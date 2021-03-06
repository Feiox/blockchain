// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "util.h"

#include "chainparamsbase.h"
#include "random.h"
#include "serialize.h"
#include "sync.h"
#include "utilstrencodings.h"
#include "utiltime.h"

#include <stdarg.h>

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#ifndef WIN32
// for posix_fallocate
#ifdef __linux__

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#endif // __linux__

#include <algorithm>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4805)
#pragma warning(disable:4717)
#endif

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501

#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501

#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <io.h> /* for _commit */
#include <shlobj.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp> // for startswith() and endswith()
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options/detail/config_file.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>

// Work around clang compilation problem in Boost 1.46:
// /usr/include/boost/program_options/detail/config_file.hpp:163:17: error: call to function 'to_internal' that is neither visible in the template definition nor found by argument-dependent lookup
// See also: http://stackoverflow.com/questions/10020179/compilation-fail-in-boost-librairies-program-options
//           http://clang.debian.net/status.php?version=3.0&key=CANNOT_FIND_FUNCTION
namespace boost {

    namespace program_options {
        std::string to_internal(const std::string&);
    }

} // namespace boost

using namespace std;

const char * const BITCOIN_CONF_FILENAME = "bitcoin.conf"; // 比特币默认配置文件名
const char * const BITCOIN_PID_FILENAME = "bitcoind.pid"; // 比特币默认 pid 文件名

map<string, string> mapArgs; // 启动选项（命令行参数，配置文件）单值映射列表，map<选项名，选项值>
map<string, vector<string> > mapMultiArgs; // 启动选项多值映射列表，map<选项名，vector<选项值> >
bool fDebug = false;
bool fPrintToConsole = false;
bool fPrintToDebugLog = true;
bool fDaemon = false;
bool fServer = false;
string strMiscWarning;
bool fLogTimestamps = DEFAULT_LOGTIMESTAMPS; // 默认为 true
bool fLogTimeMicros = DEFAULT_LOGTIMEMICROS;
bool fLogIPs = DEFAULT_LOGIPS;
volatile bool fReopenDebugLog = false; // 再次打开日志文件标志，默认关闭
CTranslationInterface translationInterface;

/** Init OpenSSL library multithreading support */
static CCriticalSection** ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line) NO_THREAD_SAFETY_ANALYSIS
{
    if (mode & CRYPTO_LOCK) {
        ENTER_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    } else {
        LEAVE_CRITICAL_SECTION(*ppmutexOpenSSL[i]);
    }
}

// Init
class CInit
{
public:
    CInit()
    {
        // Init OpenSSL library multithreading support
        ppmutexOpenSSL = (CCriticalSection**)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(CCriticalSection*));
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            ppmutexOpenSSL[i] = new CCriticalSection();
        CRYPTO_set_locking_callback(locking_callback);

        // OpenSSL can optionally load a config file which lists optional loadable modules and engines.
        // We don't use them so we don't require the config. However some of our libs may call functions
        // which attempt to load the config file, possibly resulting in an exit() or crash if it is missing
        // or corrupt. Explicitly tell OpenSSL not to try to load the file. The result for our libs will be
        // that the config appears to have been loaded and there are no modules/engines available.
        OPENSSL_no_config();

#ifdef WIN32
        // Seed OpenSSL PRNG with current contents of the screen
        RAND_screen();
#endif

        // Seed OpenSSL PRNG with performance counter
        RandAddSeed();
    }
    ~CInit()
    {
        // Securely erase the memory used by the PRNG
        RAND_cleanup();
        // Shutdown OpenSSL library multithreading support
        CRYPTO_set_locking_callback(NULL);
        for (int i = 0; i < CRYPTO_num_locks(); i++)
            delete ppmutexOpenSSL[i];
        OPENSSL_free(ppmutexOpenSSL);
    }
}
instance_of_cinit;

/**
 * LogPrintf() has been broken a couple of times now
 * by well-meaning people adding mutexes in the most straightforward way.
 * It breaks because it may be called by global destructors during shutdown.
 * Since the order of destruction of static/global objects is undefined,
 * defining a mutex as a global object doesn't work (the mutex gets
 * destroyed, and then some later destructor calls OutputDebugStringF,
 * maybe indirectly, and you get a core dump at shutdown trying to lock
 * the mutex).
 */

static boost::once_flag debugPrintInitFlag = BOOST_ONCE_INIT;

/**
 * We use boost::call_once() to make sure mutexDebugLog and
 * vMsgsBeforeOpenLog are initialized in a thread-safe manner.
 *
 * NOTE: fileout, mutexDebugLog and sometimes vMsgsBeforeOpenLog
 * are leaked on exit. This is ugly, but will be cleaned up by
 * the OS/libc. When the shutdown sequence is fully audited and
 * tested, explicit destruction of these objects can be implemented.
 */ // 我们使用 boost::call_once() 确保 mutexDebugLog 和 vMsgsBeforeOpenLog 以线程安全的方式初始化。
static FILE* fileout = NULL; // 日志文件指针
static boost::mutex* mutexDebugLog = NULL; // 日志文件锁
static list<string> *vMsgsBeforeOpenLog; // 打开日志文件前的消息链表

static int FileWriteStr(const std::string &str, FILE *fp)
{
    return fwrite(str.data(), 1, str.size(), fp); // 写入字符串到文件指针关联的文件
}

static void DebugPrintInit() // 初始化调试日志文件锁
{
    assert(mutexDebugLog == NULL); // 若调试日志锁为空
    mutexDebugLog = new boost::mutex(); // 新建一个互斥锁
    vMsgsBeforeOpenLog = new list<string>; // 新建一个字符串类型的链表
}

void OpenDebugLog()
{
    boost::call_once(&DebugPrintInit, debugPrintInitFlag); // 确保只执行 DebugPrintInit() 一次
    boost::mutex::scoped_lock scoped_lock(*mutexDebugLog); // 上锁

    assert(fileout == NULL); // 文件指针检测，确保未初始化
    assert(vMsgsBeforeOpenLog); // 确保打开日志文件前的消息链表存在
    boost::filesystem::path pathDebug = GetDataDir() / "debug.log"; // 获取调试文件位置
    fileout = fopen(pathDebug.string().c_str(), "a"); // 以追加只写的方式打开，若文件不存在则创建
    if (fileout) setbuf(fileout, NULL); // unbuffered // 设置无缓冲

    // dump buffered messages from before we opened the log // 导出在我们打开日志前缓冲的消息
    while (!vMsgsBeforeOpenLog->empty()) { // 若消息链表非空，遍历该链表
        FileWriteStr(vMsgsBeforeOpenLog->front(), fileout); // 把一个消息字符串写入日志文件
        vMsgsBeforeOpenLog->pop_front(); // 链表头出链
    }

    delete vMsgsBeforeOpenLog; // 删除该链表
    vMsgsBeforeOpenLog = NULL; // 指针置空，防止出现野指针
}

bool LogAcceptCategory(const char* category)
{
    if (category != NULL) // 若类型非空
    {
        if (!fDebug) // 若调试选项未开启
            return false; // 直接返回 false

        // Give each thread quick access to -debug settings. // 让每个线程快速访问 -debug 选项设置。
        // This helps prevent issues debugging global destructors, // 这有助于防止调试全局析构函数的问题，
        // where mapMultiArgs might be deleted before another // mapMultiArgs 可能在另一个全局析构函数
        // global destructor calls LogPrint() // 调用 LogPrint() 之前被删除
        static boost::thread_specific_ptr<set<string> > ptrCategory; // 线程局部存储（TLS）为每个线程独有
        if (ptrCategory.get() == NULL) // 初始为空
        {
            const vector<string>& categories = mapMultiArgs["-debug"]; // 获取调试选项指定的值（调试内容）存入类型列表
            ptrCategory.reset(new set<string>(categories.begin(), categories.end())); // 获取类型列表每个元素的地址存入 TLS 中
            // thread_specific_ptr automatically deletes the set when the thread ends.
        } // thread_specific_ptr 在线程结束时自动删除该集合。RAII 技术。
        const set<string>& setCategories = *ptrCategory.get(); // 获取类别字符串集合的引用

        // if not debugging everything and not debugging specific category, LogPrint does nothing. // 如果不调试全部内容而调试特定类别，LogPrint 什么也不做。
        if (setCategories.count(string("")) == 0 && // 若类别集中含有空串
            setCategories.count(string("1")) == 0 && // 且含有字符串 “1”
            setCategories.count(string(category)) == 0) // 且含有指定类别
            return false; // 直接返回 false
    }
    return true; // 返回 true
}

/**
 * fStartedNewLine is a state variable held by the calling context that will
 * suppress printing of the timestamp when multiple calls are made that don't
 * end in a newline. Initialize it to true, and hold it, in the calling context.
 */ // fStartedNewLine 是一个调用上下文保存的状态变量，它将在多次调用不以换行符结束时禁止打印时间戳。初始化为 true，并在调用上下文中保存该值。
static std::string LogTimestampStr(const std::string &str, bool *fStartedNewLine)
{
    string strStamped; // 保存打上时间戳的字符串

    if (!fLogTimestamps) // 记录时间戳标志若为 false
        return str; // 直接返回该字符串

    if (*fStartedNewLine) { // 换行标志，默认为 true
        int64_t nTimeMicros = GetLogTimeMicros(); // 获取当前时间，微秒
        strStamped = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nTimeMicros/1000000); // 转换为秒，并格式化日期时间字符串
        if (fLogTimeMicros) // 若记录微秒时间
            strStamped += strprintf(".%06d", nTimeMicros%1000000); // 追加微秒到时间戳
        strStamped += ' ' + str; // 空格隔开拼接字符串
    } else // 否则
        strStamped = str; // 不打时间戳

    if (!str.empty() && str[str.size()-1] == '\n') // 若字符串非空 且 最后一个字符为换行符
        *fStartedNewLine = true; // 换行标志置为 true
    else // 若字符串为空
        *fStartedNewLine = false; // 换行标志置为 false

    return strStamped; // 返回打上时间戳的字符串
}

int LogPrintStr(const std::string &str)
{
    int ret = 0; // Returns total number of characters written // 返回写入字符的总数
    static bool fStartedNewLine = true; // 开始新的一行标志，初始化为 true

    string strTimestamped = LogTimestampStr(str, &fStartedNewLine); // 把字符串加上时间戳

    if (fPrintToConsole) // 若输出到控制台选项开启
    {
        // print to console // 输出到控制台
        ret = fwrite(strTimestamped.data(), 1, strTimestamped.size(), stdout); // 把数据写入标准输出
        fflush(stdout); // 刷新标准输出
    }
    else if (fPrintToDebugLog) // 若输出到调试日志选项开启
    {
        boost::call_once(&DebugPrintInit, debugPrintInitFlag); // 注册只调用一次调试打印初始化
        boost::mutex::scoped_lock scoped_lock(*mutexDebugLog); // 区域锁

        // buffer if we haven't opened the log yet // 如果我们还未打开日志，进行缓冲
        if (fileout == NULL) { // 若文件指针为空
            assert(vMsgsBeforeOpenLog); // 检查消息链表已创建完毕
            ret = strTimestamped.length(); // 获取打上时间戳的字符串长度
            vMsgsBeforeOpenLog->push_back(strTimestamped); // 加入该消息链表
        }
        else // 若已经打开
        {
            // reopen the log file, if requested // 若有需求，再次打开日志文件
            if (fReopenDebugLog) { // 若指定在再次打开日志文件
                fReopenDebugLog = false; // 该标志先置为 false
                boost::filesystem::path pathDebug = GetDataDir() / "debug.log"; // 获取日志文件的路径
                if (freopen(pathDebug.string().c_str(),"a",fileout) != NULL) // 再次打开日志文件，以追加的方式打开
                    setbuf(fileout, NULL); // unbuffered // 关闭该文件指针的缓冲机制
            }

            ret = FileWriteStr(strTimestamped, fileout); // 把打上时间戳的字符串写入日志文件
        }
    }
    return ret; // 返回写入调试日志文件的字符总数
}

/** Interpret string as boolean, for argument parsing */
static bool InterpretBool(const std::string& strValue) // 把字符串转换为布尔型，用于参数解析
{
    if (strValue.empty()) // 若为空串
        return true; // 返回 true，表示指定的选项未指定值时，该值默认为 true
    return (atoi(strValue) != 0); // 否则，在返回时转换为对应布尔型
}

/** Turn -noX into -X=0 */ // 转换 -noX 为 -X=0
static void InterpretNegativeSetting(std::string& strKey, std::string& strValue)
{
    if (strKey.length()>3 && strKey[0]=='-' && strKey[1]=='n' && strKey[2]=='o') // 若选项名长度大于 3，且满足所示条件
    {
        strKey = "-" + strKey.substr(3); // 重构选项名
        strValue = InterpretBool(strValue) ? "0" : "1"; // 设置选项值
    }
}

void ParseParameters(int argc, const char* const argv[]) // 3.1.0.解析命令行参数
{
    mapArgs.clear(); // 1.清空启动选项单值映射列表
    mapMultiArgs.clear(); // 清空启动选项多值映射列表

    for (int i = 1; i < argc; i++) // 2.从第一个命令行参数开始，遍历命令行参数指针数组
    {
        std::string str(argv[i]); // 2.1.获取一个命令参数：选项名=选项值
        std::string strValue; // 用于保存选项值
        size_t is_index = str.find('='); // 找到等号的位置
        if (is_index != std::string::npos) // 若存在等号
        {
            strValue = str.substr(is_index+1); // 截取选项值子串
            str = str.substr(0, is_index); // 截取选项名子串
        }
#ifdef WIN32 // 2.2.windows 平台
        boost::to_lower(str); // 选项名转换为小写
        if (boost::algorithm::starts_with(str, "/")) // 若选项名以字符 "/" 开头
            str = "-" + str.substr(1); // 替换开头为字符 "-"
#endif

        if (str[0] != '-') // 2.3.若选项名不以字符 '-' 开头
            break; // 跳出，丢弃该选项

        // Interpret --foo as -foo. // 转换 --foo 为 -foo。
        // If both --foo and -foo are set, the last takes effect. // 若同时设置了 --foo 和 -foo，则后者生效。
        if (str.length() > 1 && str[1] == '-') // 若选项名长度大于 1 且 第二个字符为 '-'
            str = str.substr(1); // 则丢弃第一个字符 '-'
        InterpretNegativeSetting(str, strValue); // 2.4.转换 -no 选项名设置

        mapArgs[str] = strValue; // 2.5.加入启动选项单值映射列表
        mapMultiArgs[str].push_back(strValue); // 加入启动选项多值映射列表
    } // 循环，直到所有命令行参数解析完毕
}

std::string GetArg(const std::string& strArg, const std::string& strDefault)
{
    if (mapArgs.count(strArg))
        return mapArgs[strArg];
    return strDefault;
}

int64_t GetArg(const std::string& strArg, int64_t nDefault)
{
    if (mapArgs.count(strArg))
        return atoi64(mapArgs[strArg]);
    return nDefault;
}

bool GetBoolArg(const std::string& strArg, bool fDefault)
{
    if (mapArgs.count(strArg)) // 若该选项存在
        return InterpretBool(mapArgs[strArg]); // 返回其对应的值（转换为布尔型）
    return fDefault; // 否则返回默认值
}

bool SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    if (mapArgs.count(strArg)) // 若该选项已经存在（设置）
        return false; // 直接返回 false
    mapArgs[strArg] = strValue; // 否则设置为指定的值
    return true; // 返回 true，表示设置成功
}

bool SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message) {
    return std::string(optIndent,' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent,' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(NULL, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "bitcoin";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
}

boost::filesystem::path GetDefaultDataDir()
{
    namespace fs = boost::filesystem;
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\Bitcoin
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\Bitcoin
    // Mac: ~/Library/Application Support/Bitcoin
    // Unix: ~/.bitcoin
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Bitcoin";
#else // Unix/Linux
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == NULL || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    pathRet /= "Library/Application Support";
    TryCreateDirectory(pathRet);
    return pathRet / "Bitcoin";
#else
    // Unix
    return pathRet / ".bitcoin";
#endif
#endif
}

static boost::filesystem::path pathCached; // 路径缓存
static boost::filesystem::path pathCachedNetSpecific; // 指定网络的路径缓存
static CCriticalSection csPathCached; // 路径缓存锁

const boost::filesystem::path &GetDataDir(bool fNetSpecific)
{
    namespace fs = boost::filesystem;

    LOCK(csPathCached); // 1.路径缓存上锁

    fs::path &path = fNetSpecific ? pathCachedNetSpecific : pathCached; // 2.false

    // This can be called during exceptions by LogPrintf(), so we cache the // 这可以在异常期间通过 LogPrintf() 调用，
    // value so we don't have to do memory allocations after that. // 所以我们缓存该值，以至于我们不用在之后进行内存分配。
    if (!path.empty()) // 3.若路径非空
        return path; // 直接返回数据目录的路径

    if (mapArgs.count("-datadir")) { // 4.否则，若指定了数据目录的位置
        path = fs::system_complete(mapArgs["-datadir"]); // 获取指定的路径
        if (!fs::is_directory(path)) { // 若该路径不是目录
            path = ""; // 置空
            return path; // 返回
        }
    } else { // 若未指定数据目录位置
        path = GetDefaultDataDir(); // 获取默认的数据目录路径
    }
    if (fNetSpecific) // false // 5.若指定了特定网络
        path /= BaseParams().DataDir(); // 路径拼接，获取不同网络的数据目录

    fs::create_directories(path); // 6.创建该目录

    return path; // 7.返回数据目录的路径
}

void ClearDatadirCache()
{
    pathCached = boost::filesystem::path(); // 路径缓存置空
    pathCachedNetSpecific = boost::filesystem::path(); // 指定网络的路径缓存置空
}

boost::filesystem::path GetConfigFile()
{
    boost::filesystem::path pathConfigFile(GetArg("-conf", BITCOIN_CONF_FILENAME)); // 获取配置文件（指定/默认）名
    if (!pathConfigFile.is_complete()) // 检查该文件名是否完整
        pathConfigFile = GetDataDir(false) / pathConfigFile; // 路径拼接，获取配置文件路径

    return pathConfigFile; // 返回配置文件路径
}

void ReadConfigFile(map<string, string>& mapSettingsRet,
                    map<string, vector<string> >& mapMultiSettingsRet)
{
    boost::filesystem::ifstream streamConfig(GetConfigFile()); // 1.获取配置文件路径并创建文件输入流对象
    if (!streamConfig.good()) // 允许初次运行没有配置文件
        return; // No bitcoin.conf file is OK

    set<string> setOptions; // 2.选择集
    setOptions.insert("*"); // 插入 "*"，用于过滤配置文件中带有 '*' 的行

    for (boost::program_options::detail::config_file_iterator it(streamConfig, setOptions), end; it != end; ++it) // 3.遍历配置文件输入流
    {
        // Don't overwrite existing settings so command line settings override bitcoin.conf // 不覆盖已存在的设置，因此命令行设置会覆盖配置文件设置
        string strKey = string("-") + it->string_key; // 3.1.选项名
        string strValue = it->value[0]; // 选项值
        InterpretNegativeSetting(strKey, strValue); // 把 -noX 转换为 -X=0
        if (mapSettingsRet.count(strKey) == 0) // 3.2.若启动选项单值映射列表中不含该选项
            mapSettingsRet[strKey] = strValue; // 插入列表
        mapMultiSettingsRet[strKey].push_back(strValue); // 插入多值映射列表
    }
    // If datadir is changed in .conf file: // 如果数据目录在配置文件中改变
    ClearDatadirCache(); // 4.清理数据目录缓存
}

#ifndef WIN32
boost::filesystem::path GetPidFile()
{
    boost::filesystem::path pathPidFile(GetArg("-pid", BITCOIN_PID_FILENAME)); // 获取 pid 文件名
    if (!pathPidFile.is_complete()) pathPidFile = GetDataDir() / pathPidFile; // pid 文件路径拼接
    return pathPidFile; // 返回 pid 文件路径名
}

void CreatePidFile(const boost::filesystem::path &path, pid_t pid)
{
    FILE* file = fopen(path.string().c_str(), "w"); // 以只写方式打开文件，若不存在则新建
    if (file) // 创建成功
    {
        fprintf(file, "%d\n", pid); // 输出 pid 到该文件
        fclose(file); // 关闭文件
    }
}
#endif

bool RenameOver(boost::filesystem::path src, boost::filesystem::path dest)
{
#ifdef WIN32
    return MoveFileExA(src.string().c_str(), dest.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directory if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectory(const boost::filesystem::path& p)
{
    try
    {
        return boost::filesystem::create_directory(p); // 创建目录 p
    } catch (const boost::filesystem::filesystem_error&) {
        if (!boost::filesystem::exists(p) || !boost::filesystem::is_directory(p)) // 目录 p 不存在 或 存在但非目录类型
            throw; // 抛出异常
    }

    // create_directory didn't create the directory, it had to have existed already
    return false;
}

void FileCommit(FILE *fileout)
{
    fflush(fileout); // harmless if redundantly called // 刷新数据到磁盘
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(fileout));
    FlushFileBuffers(hFile);
#else
    #if defined(__linux__) || defined(__NetBSD__)
    fdatasync(fileno(fileout));
    #elif defined(__APPLE__) && defined(F_FULLFSYNC)
    fcntl(fileno(fileout), F_FULLFSYNC, 0);
    #else
    fsync(fileno(fileout));
    #endif
#endif
}

bool TruncateFile(FILE *file, unsigned int length) {
#if defined(WIN32) // windows
    return _chsize(_fileno(file), length) == 0;
#else // Unix/Linux
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 */
int RaiseFileDescriptorLimit(int nMinFD) {
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) { // RLIMIT_NOFILE 内核默认 1024，表示每个进程最大能打开的文件描述符数量，linux-4.16.4 src 中定义为 5、6、7
        if (limitFD.rlim_cur < (rlim_t)nMinFD) { // rlim_cur 为 Soft limit 是内核对一个进程能使用的资源的限制，而 rlim_max 为 Hard limit 是 Soft limit 的上限
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur; // 返回调整后的描述符限制
    }
    return nMinFD; // getrlimit failed, assume it's fine
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length) {
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif defined(__linux__)
    // Version using posix_fallocate
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    fseek(file, offset, SEEK_SET);
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

void ShrinkDebugFile()
{
    // Scroll debug.log if it's getting too big // 若它变得太大，回滚 debug.log
    boost::filesystem::path pathLog = GetDataDir() / "debug.log"; // 获取日志位置
    FILE* file = fopen(pathLog.string().c_str(), "r"); // 以只读方式打开日志
    if (file && boost::filesystem::file_size(pathLog) > 10 * 1000000) // 若日志文件大小超过约 10MiB
    {
        // Restart the file with some of the end // 使用结尾信息重写文件
        std::vector <char> vch(200000,0); // 开辟 200KB 容器并初始化为 0
        fseek(file, -((long)vch.size()), SEEK_END); // 文件指针从文件尾部向前偏移 200,000 个字节
        int nBytes = fread(begin_ptr(vch), 1, vch.size(), file); // 读取最新的 200KB 调试日志到内存
        fclose(file); // 关闭文件

        file = fopen(pathLog.string().c_str(), "w"); // 以只写方式重新打开文件，文件存在长度清零
        if (file) // 若打开成功
        {
            fwrite(begin_ptr(vch), 1, nBytes, file); // 把最新的 200KB 调试日志写入文件
            fclose(file); // 关闭文件
        }
    }
    else if (file != NULL) // 若打开成功
        fclose(file); // 直接关闭文件
}

#ifdef WIN32
boost::filesystem::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    namespace fs = boost::filesystem;

    char pszPath[MAX_PATH] = "";

    if(SHGetSpecialFolderPathA(NULL, pszPath, nFolder, fCreate))
    {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathA() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

boost::filesystem::path GetTempPath() {
#if BOOST_FILESYSTEM_VERSION == 3
    return boost::filesystem::temp_directory_path();
#else
    // TODO: remove when we don't support filesystem v2 anymore
    boost::filesystem::path path;
#ifdef WIN32
    char pszPath[MAX_PATH] = "";

    if (GetTempPathA(MAX_PATH, pszPath))
        path = boost::filesystem::path(pszPath);
#else
    path = boost::filesystem::path("/tmp");
#endif
    if (path.empty() || !boost::filesystem::is_directory(path)) {
        LogPrintf("GetTempPath(): failed to find temp path\n");
        return boost::filesystem::path("");
    }
    return path;
#endif
}

void runCommand(const std::string& strCommand)
{
    int nErr = ::system(strCommand.c_str()); // 执行命令 bash 命令
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}

void RenameThread(const char* name)
{
#if defined(PR_SET_NAME) // Linux
    // Only the first 15 characters are used (16 - NUL terminator) // 仅用前 15 个字符（16 - NULL 终止符）
    ::prctl(PR_SET_NAME, name, 0, 0, 0); // 设置线程名为 name，命名超出 15 个字符的部分会被静默截断
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)) // UNIX
    pthread_set_name_np(pthread_self(), name);

#elif defined(MAC_OSX) // Apple
    pthread_setname_np(name);
#else
    // Prevent warnings for unused parameters... // 防止对未使用的参数的警告
    (void)name; // 转为空
#endif
}

void SetupEnvironment()
{
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale // 在多数系统（例如：Linux，而非 BSD）上，环境的区域设置（场所或地点）可能无效，
    // may be invalid, in which case the "C" locale is used as fallback. // “C” 区域设置用于后备。
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__) // 若非（为定义） WIN32、MAC_OSX、__FreeBSD__、__OpenBSD__
    try { // 1.尝试进行本地区域设置
        std::locale(""); // Raises a runtime error if current locale is invalid // 若当前区域设置无效，则导致运行时错误
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1); // POSIX 接口，回退到 “C” 环境变量
    }
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors // 路径区域设置是懒加载的，且为了避免在多线程环境中的反初始化错误，
    // in multithreading environments, it is set explicitly by the main thread. // 它通过主线程显示设置。
    // A dummy locale is used to extract the internal default locale, used by // 虚拟区域设置通过使用 boost::filesystem::path 用于提取内部默认的区域设置，
    // boost::filesystem::path, which is then used to explicitly imbue the path. // 然后用于显示填充路径。
    std::locale loc = boost::filesystem::path::imbue(std::locale::classic()); // 2.先设置一个虚假的用于提取出原有设置
    boost::filesystem::path::imbue(loc); // 2.再填充
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets // 初始化 Windows 套接字
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true; // 非 WIN32 系统直接返回 true
}

void SetThreadPriority(int nPriority)
{
#ifdef WIN32
    SetThreadPriority(GetCurrentThread(), nPriority);
#else // WIN32
#ifdef PRIO_THREAD
    setpriority(PRIO_THREAD, 0, nPriority);
#else // PRIO_THREAD
    setpriority(PRIO_PROCESS, 0, nPriority);
#endif // PRIO_THREAD
#endif // WIN32
}

int GetNumCores()
{
#if BOOST_VERSION >= 105600
    return boost::thread::physical_concurrency();
#else // Must fall back to hardware_concurrency, which unfortunately counts virtual cores
    return boost::thread::hardware_concurrency();
#endif
}

