// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "chain.h"
#include "coins.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "merkleblock.h"
#include "net.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "rpcserver.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "txmempool.h"
#include "uint256.h"
#include "utilstrencodings.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

using namespace std;

void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex)
{
    txnouttype type;
    vector<CTxDestination> addresses;
    int nRequired;

    out.push_back(Pair("asm", ScriptToAsmStr(scriptPubKey))); // 脚本操作码
    if (fIncludeHex)
        out.push_back(Pair("hex", HexStr(scriptPubKey.begin(), scriptPubKey.end()))); // 16 进制形式

    if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        out.push_back(Pair("type", GetTxnOutputType(type))); // 脚本类型
        return;
    }

    out.push_back(Pair("reqSigs", nRequired)); // 是否需要签名
    out.push_back(Pair("type", GetTxnOutputType(type))); // 类型

    UniValue a(UniValue::VARR);
    BOOST_FOREACH(const CTxDestination& addr, addresses)
        a.push_back(CBitcoinAddress(addr).ToString());
    out.push_back(Pair("addresses", a)); // 输出地址
}

void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry)
{
    entry.push_back(Pair("txid", tx.GetHash().GetHex()));
    entry.push_back(Pair("size", (int)::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION)));
    entry.push_back(Pair("version", tx.nVersion));
    entry.push_back(Pair("locktime", (int64_t)tx.nLockTime));
    UniValue vin(UniValue::VARR);
    BOOST_FOREACH(const CTxIn& txin, tx.vin) {
        UniValue in(UniValue::VOBJ);
        if (tx.IsCoinBase())
            in.push_back(Pair("coinbase", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        else {
            in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
            in.push_back(Pair("vout", (int64_t)txin.prevout.n));
            UniValue o(UniValue::VOBJ);
            o.push_back(Pair("asm", ScriptToAsmStr(txin.scriptSig, true)));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            in.push_back(Pair("scriptSig", o));
        }
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("n", (int64_t)i));
        UniValue o(UniValue::VOBJ);
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        vout.push_back(out);
    }
    entry.push_back(Pair("vout", vout));

    if (!hashBlock.IsNull()) {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("time", pindex->GetBlockTime()));
                entry.push_back(Pair("blocktime", pindex->GetBlockTime()));
            }
            else
                entry.push_back(Pair("confirmations", 0));
        }
    }
}

UniValue getrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2) // 参数为 1 或 2 个
        throw runtime_error( // 命令帮助反馈
            "getrawtransaction \"txid\" ( verbose )\n"
            "\nNOTE: By default this function only works sometimes. This is when the tx is in the mempool\n"
            "or there is an unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option.\n"
            "\nReturn the raw transaction data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'txid'.\n"
            "If verbose is non-zero, returns an Object with information about 'txid'.\n"

            "\nArguments:\n"
            "1. \"txid\"      (string, required) The transaction id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'txid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",       (string) The serialized, hex-encoded data for 'txid'\n"
            "  \"txid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"size\" : n,             (numeric) The transaction size\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) \n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n      (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"bitcoinaddress\"        (string) bitcoin address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,      (numeric) The confirmations\n"
            "  \"time\" : ttt,             (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"blocktime\" : ttt         (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawtransaction", "\"mytxid\"")
            + HelpExampleCli("getrawtransaction", "\"mytxid\" 1")
            + HelpExampleRpc("getrawtransaction", "\"mytxid\", 1")
        );

    LOCK(cs_main); // 上锁

    uint256 hash = ParseHashV(params[0], "parameter 1"); // 解析指定的交易哈希

    bool fVerbose = false; // 详细信息标志，默认为 false
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0); // 获取详细信息设置

    CTransaction tx;
    uint256 hashBlock;
    if (!GetTransaction(hash, tx, Params().GetConsensus(), hashBlock, true)) // 获取交易及所在区块哈希
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");

    string strHex = EncodeHexTx(tx); // 编码交易

    if (!fVerbose) // 若为 false
        return strHex; // 直接返回编码后的数据

    UniValue result(UniValue::VOBJ); // 否则，构建对象类型返回结果
    result.push_back(Pair("hex", strHex)); // 加入序列化的交易
    TxToJSON(tx, hashBlock, result); // 交易信息转换为 JSON 格式加入结果
    return result; // 返回结果
}

UniValue gettxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 1 && params.size() != 2)) // 参数为 1 个或 2 个
        throw runtime_error( // 命令帮助反馈
            "gettxoutproof [\"txid\",...] ( blockhash )\n"
            "\nReturns a hex-encoded proof that \"txid\" was included in a block.\n"
            "\nNOTE: By default this function only works sometimes. This is when there is an\n"
            "unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option or\n"
            "specify the block in which the transaction is included in manually (by blockhash).\n"
            "\nReturn the raw transaction data.\n"
            "\nArguments:\n"
            "1. \"txids\"       (string) A json array of txids to filter\n"
            "    [\n"
            "      \"txid\"     (string) A transaction hash\n"
            "      ,...\n"
            "    ]\n"
            "2. \"block hash\"  (string, optional) If specified, looks for txid in the block with this hash\n"
            "\nResult:\n"
            "\"data\"           (string) A string that is a serialized, hex-encoded data for the proof.\n"
        );

    set<uint256> setTxids; // 交易索引集合
    uint256 oneTxid;
    UniValue txids = params[0].get_array(); // 获取指定的交易索引集
    for (unsigned int idx = 0; idx < txids.size(); idx++) { // 遍历该集合
        const UniValue& txid = txids[idx]; // 获取交易索引
        if (txid.get_str().length() != 64 || !IsHex(txid.get_str())) // 长度及 16 进制验证
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid txid ")+txid.get_str());
        uint256 hash(uint256S(txid.get_str()));
        if (setTxids.count(hash)) // 保证只插入一次
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated txid: ")+txid.get_str());
       setTxids.insert(hash); // 加入交易索引集
       oneTxid = hash; // 记录最后一笔交易哈希
    }

    LOCK(cs_main); // 上锁

    CBlockIndex* pblockindex = NULL;

    uint256 hashBlock;
    if (params.size() > 1) // 指定了区块哈希
    {
        hashBlock = uint256S(params[1].get_str()); // 获取指定区块哈希
        if (!mapBlockIndex.count(hashBlock)) // 若区块索引映射中没有该区块
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found"); // 报错
        pblockindex = mapBlockIndex[hashBlock]; // 获取区块索引
    } else { // 未指定区块
        CCoins coins;
        if (pcoinsTip->GetCoins(oneTxid, coins) && coins.nHeight > 0 && coins.nHeight <= chainActive.Height())
            pblockindex = chainActive[coins.nHeight]; // 获取该交易所在的区块索引
    }

    if (pblockindex == NULL) // 若区块索引不存在
    {
        CTransaction tx;
        if (!GetTransaction(oneTxid, tx, Params().GetConsensus(), hashBlock, false) || hashBlock.IsNull())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not yet in block");
        if (!mapBlockIndex.count(hashBlock)) // 区块索引不在区块索引映射列表中
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Transaction index corrupt");
        pblockindex = mapBlockIndex[hashBlock]; // 获取区块索引
    }

    CBlock block;
    if(!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus())) // 通过区块索引从磁盘读区块数据到 block
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");

    unsigned int ntxFound = 0; // 找到交易的个数
    BOOST_FOREACH(const CTransaction&tx, block.vtx) // 遍历区块交易列表
        if (setTxids.count(tx.GetHash())) // 若该交易在指定的交易集中
            ntxFound++; // +1
    if (ntxFound != setTxids.size()) // 找到交易个数必须等于交易集大小，及指定交易必须全部找到
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "(Not all) transactions not found in specified block");

    CDataStream ssMB(SER_NETWORK, PROTOCOL_VERSION); // 创建数据流对象
    CMerkleBlock mb(block, setTxids); // 把交易索引集以及对应区块的数据构建一个 CMerkleBlock 对象
    ssMB << mb; // 导入数据流
    std::string strHex = HexStr(ssMB.begin(), ssMB.end()); // 转换为 16 进制
    return strHex; // 返回结果
}

UniValue verifytxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1) // 参数必须为 1 个
        throw runtime_error( // 命令帮助反馈
            "verifytxoutproof \"proof\"\n"
            "\nVerifies that a proof points to a transaction in a block, returning the transaction it commits to\n"
            "and throwing an RPC error if the block is not in our best chain\n"
            "\nArguments:\n"
            "1. \"proof\"    (string, required) The hex-encoded proof generated by gettxoutproof\n"
            "\nResult:\n"
            "[\"txid\"]      (array, strings) The txid(s) which the proof commits to, or empty array if the proof is invalid\n"
        );

    CDataStream ssMB(ParseHexV(params[0], "proof"), SER_NETWORK, PROTOCOL_VERSION); // 获取指定交易证明初始化数据流对象
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock; // 导出到 CMerkleBlock 对象中

    UniValue res(UniValue::VARR); // 数组类型的返回对象

    vector<uint256> vMatch; // 用于保存交易索引
    if (merkleBlock.txn.ExtractMatches(vMatch) != merkleBlock.header.hashMerkleRoot) // 提取交易索引列表
        return res;

    LOCK(cs_main); // 上锁

    if (!mapBlockIndex.count(merkleBlock.header.GetHash()) || !chainActive.Contains(mapBlockIndex[merkleBlock.header.GetHash()])) // 区块索引映射列表中包含该区块（头）索引 且 激活的链包含该区块
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");

    BOOST_FOREACH(const uint256& hash, vMatch) // 遍历交易索引列表
        res.push_back(hash.GetHex()); // 加入结果集
    return res; // 返回结果
}

UniValue createrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3) // 1.参数为 2 或 3 个
        throw runtime_error( // 命令帮助反馈
            "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] {\"address\":amount,\"data\":\"hex\",...} ( locktime )\n"
            "\nCreate a transaction spending the given inputs and creating new outputs.\n"
            "Outputs can be addresses or data.\n"
            "Returns hex-encoded raw transaction.\n"
            "Note that the transaction's inputs are not signed, and\n"
            "it is not stored in the wallet or transmitted to the network.\n"

            "\nArguments:\n"
            "1. \"transactions\"        (string, required) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"id\",    (string, required) The transaction id\n"
            "         \"vout\":n        (numeric, required) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "2. \"outputs\"             (string, required) a json object with outputs\n"
            "    {\n"
            "      \"address\": x.xxx   (numeric or string, required) The key is the bitcoin address, the numeric value (can be string) is the " + CURRENCY_UNIT + " amount\n"
            "      \"data\": \"hex\",     (string, required) The key is \"data\", the value is hex encoded data\n"
            "      ...\n"
            "    }\n"
            "3. locktime                (numeric, optional, default=0) Raw locktime. Non-0 value also locktime-activates inputs\n"
            "\nResult:\n"
            "\"transaction\"            (string) hex string of the transaction\n"

            "\nExamples\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"{\\\"address\\\":0.01}\"")
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"{\\\"data\\\":\\\"00010203\\\"}\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"{\\\"address\\\":0.01}\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"{\\\"data\\\":\\\"00010203\\\"}\"")
        );

    LOCK(cs_main); // 2.上锁
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VARR)(UniValue::VOBJ)(UniValue::VNUM), true); // 3.检查参数类型
    if (params[0].isNull() || params[1].isNull()) // 输入和输出均不能为空
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, arguments 1 and 2 must be non-null");

    UniValue inputs = params[0].get_array(); // 获取输入
    UniValue sendTo = params[1].get_obj(); // 获取输出

    CMutableTransaction rawTx; // 4.创建一笔原始交易

    if (params.size() > 2 && !params[2].isNull()) { // 4.1.若指定了锁定时间
        int64_t nLockTime = params[2].get_int64(); // 获取锁定时间
        if (nLockTime < 0 || nLockTime > std::numeric_limits<uint32_t>::max()) // 锁定时间范围检查
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, locktime out of range");
        rawTx.nLockTime = nLockTime; // 交易锁定时间初始化
    }

    for (unsigned int idx = 0; idx < inputs.size(); idx++) { // 4.2.遍历输入，构建原始交易输入列表
        const UniValue& input = inputs[idx]; // 获取一个输入
        const UniValue& o = input.get_obj(); // 拿到该输入对象

        uint256 txid = ParseHashO(o, "txid"); // 获取交易索引

        const UniValue& vout_v = find_value(o, "vout"); // 获取输出序号
        if (!vout_v.isNum()) // 输出序号必须为数字
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int(); // 获取该数字
        if (nOutput < 0) // 输出索引最小为 0
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        uint32_t nSequence = (rawTx.nLockTime ? std::numeric_limits<uint32_t>::max() - 1 : std::numeric_limits<uint32_t>::max()); // 锁定时间
        CTxIn in(COutPoint(txid, nOutput), CScript(), nSequence); // 构建一个交易输入对象

        rawTx.vin.push_back(in); // 加入原始交易输入列表
    }

    set<CBitcoinAddress> setAddress; // 地址集
    vector<string> addrList = sendTo.getKeys(); // 获取输出的所有关键字（地址）
    BOOST_FOREACH(const string& name_, addrList) { // 4.3.遍历地址列表

        if (name_ == "data") { // 若关键字中包含 "data"
            std::vector<unsigned char> data = ParseHexV(sendTo[name_].getValStr(),"Data"); // 解析数据

            CTxOut out(0, CScript() << OP_RETURN << data); // 构建交易输出对象
            rawTx.vout.push_back(out); // 加入原始交易输出列表
        } else { // 否则为目的地址
            CBitcoinAddress address(name_); // 构建比特币地址
            if (!address.IsValid()) // 检验地址是否有效
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Bitcoin address: ")+name_);

            if (setAddress.count(address)) // 保证地址集中不存在该地址，防止地址重复输入
                throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
            setAddress.insert(address); // 插入地址集

            CScript scriptPubKey = GetScriptForDestination(address.Get()); // 从目的地址获取脚本公钥
            CAmount nAmount = AmountFromValue(sendTo[name_]); // 获取金额

            CTxOut out(nAmount, scriptPubKey); // 构建交易输出对象
            rawTx.vout.push_back(out); // 加入原始交易输出列表
        }
    }

    return EncodeHexTx(rawTx); // 5.16 进制编码该原始交易并返回
}

UniValue decoderawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1) // 参数必须为 1 个
        throw runtime_error( // 命令帮助反馈
            "decoderawtransaction \"hexstring\"\n"
            "\nReturn a JSON object representing the serialized, hex-encoded transaction.\n"

            "\nArguments:\n"
            "1. \"hex\"      (string, required) The transaction hex string\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"id\",        (string) The transaction id\n"
            "  \"size\" : n,             (numeric) The transaction size\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) The output number\n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n     (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [             (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"12tvKAXCxZjSmdNbao16dKXC8tRWfcF5oc\"   (string) bitcoin address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawtransaction", "\"hexstring\"")
            + HelpExampleRpc("decoderawtransaction", "\"hexstring\"")
        );

    LOCK(cs_main); // 上锁
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)); // 检测参数类型

    CTransaction tx;

    if (!DecodeHexTx(tx, params[0].get_str())) // 解码交易
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    UniValue result(UniValue::VOBJ);
    TxToJSON(tx, uint256(), result); // 把交易信息转换为 JSON 加入结果对象

    return result; // 返回结果对象
}

UniValue decodescript(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1) // 参数必须是 1 个
        throw runtime_error( // 命令帮助反馈
            "decodescript \"hex\"\n"
            "\nDecode a hex-encoded script.\n"
            "\nArguments:\n"
            "1. \"hex\"     (string) the hex encoded script\n"
            "\nResult:\n"
            "{\n"
            "  \"asm\":\"asm\",   (string) Script public key\n"
            "  \"hex\":\"hex\",   (string) hex encoded public key\n"
            "  \"type\":\"type\", (string) The output type\n"
            "  \"reqSigs\": n,    (numeric) The required signatures\n"
            "  \"addresses\": [   (json array of string)\n"
            "     \"address\"     (string) bitcoin address\n"
            "     ,...\n"
            "  ],\n"
            "  \"p2sh\",\"address\" (string) script address\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("decodescript", "\"hexstring\"")
            + HelpExampleRpc("decodescript", "\"hexstring\"")
        );

    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)); // 参数类型检查

    UniValue r(UniValue::VOBJ);
    CScript script;
    if (params[0].get_str().size() > 0){ // 若脚本非空字符串
        vector<unsigned char> scriptData(ParseHexV(params[0], "argument")); // 解析参数
        script = CScript(scriptData.begin(), scriptData.end()); // 构建序列化的脚本
    } else { // 空脚本是有效的
        // Empty scripts are valid
    }
    ScriptPubKeyToJSON(script, r, false); // 脚本公钥转换为 JSON 格式加入结果集

    r.push_back(Pair("p2sh", CBitcoinAddress(CScriptID(script)).ToString())); // Base58 编码的脚本哈希
    return r; // 返回结果集
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxInErrorToJSON(const CTxIn& txin, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.push_back(Pair("txid", txin.prevout.hash.ToString()));
    entry.push_back(Pair("vout", (uint64_t)txin.prevout.n));
    entry.push_back(Pair("scriptSig", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
    entry.push_back(Pair("sequence", (uint64_t)txin.nSequence));
    entry.push_back(Pair("error", strMessage));
    vErrorsRet.push_back(entry);
}

UniValue signrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 4) // 1.参数最少 1 个，至多 4 个
        throw runtime_error( // 命令帮助反馈
            "signrawtransaction \"hexstring\" ( [{\"txid\":\"id\",\"vout\":n,\"scriptPubKey\":\"hex\",\"redeemScript\":\"hex\"},...] [\"privatekey1\",...] sighashtype )\n"
            "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of previous transaction outputs that\n"
            "this transaction depends on but may not yet be in the block chain.\n"
            "The third optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"prevtxs\"       (string, optional) An json array of previous dependent transaction outputs\n"
            "     [               (json array of json objects, or 'null' if none provided)\n"
            "       {\n"
            "         \"txid\":\"id\",             (string, required) The transaction id\n"
            "         \"vout\":n,                  (numeric, required) The output number\n"
            "         \"scriptPubKey\": \"hex\",   (string, required) script key\n"
            "         \"redeemScript\": \"hex\"    (string, required for P2SH) redeem script\n"
            "       }\n"
            "       ,...\n"
            "    ]\n"
            "3. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"
            "4. \"sighashtype\"     (string, optional, default=ALL) The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\"\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\" : \"value\",           (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                 (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",           (string) The hash of the referenced, previous transaction\n"
            "      \"vout\" : n,                (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",       (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,            (numeric) Script sequence number\n"
            "      \"error\" : \"text\"           (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL); // 2.钱包上锁
#else
    LOCK(cs_main);
#endif
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VARR)(UniValue::VARR)(UniValue::VSTR), true); // 3.检查参数类型

    vector<unsigned char> txData(ParseHexV(params[0], "argument 1")); // 解析第一个参数
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION); // 创建数据流对象
    vector<CMutableTransaction> txVariants; // 可变的交易列表
    while (!ssData.empty()) { // 当数据流对象非空
        try {
            CMutableTransaction tx; // 可变版本的交易
            ssData >> tx; // 导入一笔交易
            txVariants.push_back(tx); // 加入交易列表
        }
        catch (const std::exception&) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
    }

    if (txVariants.empty()) // 列表非空，至少有一笔交易
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing transaction");

    // mergedTx will end up with all the signatures; it // mergeTx 将以全部签名为结尾；
    // starts as a clone of the rawtx: // 它作为 rawtx 的副本开始：
    CMutableTransaction mergedTx(txVariants[0]); // 合并的可变交易输入集

    // Fetch previous transactions (inputs): // 获取之前的交易（输入）：
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    { // 开始访问内存池
        LOCK(mempool.cs); // 交易内存池上锁
        CCoinsViewCache &viewChain = *pcoinsTip; // 获取激活的 CCoinsView
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedTx.vin) { // 遍历交易输入列表
            const uint256& prevHash = txin.prevout.hash; // 获取交易输入的前一笔交易哈希
            CCoins coins;
            view.AccessCoins(prevHash); // this is certainly allowed to fail // 这里肯定会失败
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long // 切换回以避免锁定内存池时间过长
    }

    bool fGivenKeys = false; // 指定密钥标志，默认为 false
    CBasicKeyStore tempKeystore; // 临时私钥库
    if (params.size() > 2 && !params[2].isNull()) { // 若指定了密钥
        fGivenKeys = true; // 标志置为 true
        UniValue keys = params[2].get_array(); // 获取密钥数组
        for (unsigned int idx = 0; idx < keys.size(); idx++) { // 遍历该数组
            UniValue k = keys[idx]; // 获取一个 base58 编码的密钥
            CBitcoinSecret vchSecret; // 比特币密钥对象
            bool fGood = vchSecret.SetString(k.get_str()); // 初始化密钥
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey(); // 获取私钥
            if (!key.IsValid()) // 验证私钥是否有效
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");
            tempKeystore.AddKey(key); // 添加到临时私钥库
        }
    }
#ifdef ENABLE_WALLET
    else if (pwalletMain)
        EnsureWalletIsUnlocked(); // 确保此时钱包处于解密状态
#endif

    // Add previous txouts given in the RPC call:
    if (params.size() > 1 && !params[1].isNull()) { // 若指定了前一笔交易输出集合，且非空
        UniValue prevTxs = params[1].get_array(); // 获取前一笔交易输出的数组
        for (unsigned int idx = 0; idx < prevTxs.size(); idx++) { // 遍历该数组
            const UniValue& p = prevTxs[idx]; // 获取一个交易输出对象
            if (!p.isObject()) // 确保是对象类型
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");

            UniValue prevOut = p.get_obj(); // 获取输出对象

            RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR)); // 参数类型检查

            uint256 txid = ParseHashO(prevOut, "txid"); // 解析交易索引

            int nOut = find_value(prevOut, "vout").get_int(); // 获取交易输出序号
            if (nOut < 0) // 序号最小为 0
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout must be positive");

            vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey")); // 解析脚本公钥
            CScript scriptPubKey(pkData.begin(), pkData.end()); // 创建一个脚本公钥对象

            {
                CCoinsModifier coins = view.ModifyCoins(txid); // 获取交易索引对应的可修改 CCoins
                if (coins->IsAvailable(nOut) && coins->vout[nOut].scriptPubKey != scriptPubKey) { // 检测输出的脚本公钥是否一致
                    string err("Previous output scriptPubKey mismatch:\n");
                    err = err + ScriptToAsmStr(coins->vout[nOut].scriptPubKey) + "\nvs:\n"+
                        ScriptToAsmStr(scriptPubKey);
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                if ((unsigned int)nOut >= coins->vout.size()) // 交易输出序号若大于等于币输出大小
                    coins->vout.resize(nOut+1); // 重新设置输出列表大小 +1
                coins->vout[nOut].scriptPubKey = scriptPubKey; // 设置输出列表中输出对应的脚本公钥
                coins->vout[nOut].nValue = 0; // we don't know the actual output value // 输出对应的值初始化为 0
            }

            // if redeemScript given and not using the local wallet (private keys // 如果给定了赎回脚本，且不使用本地钱包（提供了私钥），
            // given), add redeemScript to the tempKeystore so it can be signed: // 添加赎回脚本到临时密钥库以至于对它签名：
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash()) { // 如果是 P2SH
                RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR)("redeemScript",UniValue::VSTR)); // 先进行参数类型检查
                UniValue v = find_value(prevOut, "redeemScript"); // 获取赎回脚本
                if (!v.isNull()) { // 脚本非空
                    vector<unsigned char> rsData(ParseHexV(v, "redeemScript"));
                    CScript redeemScript(rsData.begin(), rsData.end()); // 创建脚本对象
                    tempKeystore.AddCScript(redeemScript); // 添加脚本到临时密钥库
                }
            }
        }
    }

#ifdef ENABLE_WALLET
    const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain); // 若提供了密钥 或 主钱包无效，则获取临时密钥库的引用
#else
    const CKeyStore& keystore = tempKeystore;
#endif

    int nHashType = SIGHASH_ALL; // 脚本哈希类型，默认为 ALL
    if (params.size() > 3 && !params[3].isNull()) { // 若指定了类型
        static map<string, int> mapSigHashValues =
            boost::assign::map_list_of
            (string("ALL"), int(SIGHASH_ALL))
            (string("ALL|ANYONECANPAY"), int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))
            (string("NONE"), int(SIGHASH_NONE))
            (string("NONE|ANYONECANPAY"), int(SIGHASH_NONE|SIGHASH_ANYONECANPAY))
            (string("SINGLE"), int(SIGHASH_SINGLE))
            (string("SINGLE|ANYONECANPAY"), int(SIGHASH_SINGLE|SIGHASH_ANYONECANPAY))
            ; // 签名哈希值类型映射列表
        string strHashType = params[3].get_str(); // 获取哈希类型
        if (mapSigHashValues.count(strHashType)) // 若在映射列表中存在指定的哈希类型
            nHashType = mapSigHashValues[strHashType]; // 设置脚本哈希类型
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash param");
    }

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Script verification errors
    UniValue vErrors(UniValue::VARR); // 数组类型的脚本验证错误集

    // Sign what we can: // 4.我们签名：
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++) { // 遍历合并的可变交易输入列表
        CTxIn& txin = mergedTx.vin[i]; // 获取一笔交易输入
        const CCoins* coins = view.AccessCoins(txin.prevout.hash); // 获取该输入依赖的前一笔交易的哈希对应的 CCoins
        if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
            TxInErrorToJSON(txin, vErrors, "Input not found or already spent");
            continue;
        }
        const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey; // 获取前一笔交易输出的脚本公钥

        txin.scriptSig.clear(); // 清空交易输入的脚本签名
        // Only sign SIGHASH_SINGLE if there's a corresponding output: // 如果有相应的输出，只签名 SIGHASH_SINGLE
        if (!fHashSingle || (i < mergedTx.vout.size()))
            SignSignature(keystore, prevPubKey, mergedTx, i, nHashType); // 签名

        // ... and merge in other signatures: // ... 接着合并其他签名：
        BOOST_FOREACH(const CMutableTransaction& txv, txVariants) { // 遍历交易列表
            txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig); // 合并所有输入签名
        }
        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(txin.scriptSig, prevPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&mergedTx, i), &serror)) { // 验证脚本签名
            TxInErrorToJSON(txin, vErrors, ScriptErrorString(serror));
        }
    }
    bool fComplete = vErrors.empty(); // 若没有错误，表示已完成

    UniValue result(UniValue::VOBJ); // 创建对象类型的结果集
    result.push_back(Pair("hex", EncodeHexTx(mergedTx))); // 合并的交易的 16 进制编码
    result.push_back(Pair("complete", fComplete)); // 是否完成签名
    if (!vErrors.empty()) {
        result.push_back(Pair("errors", vErrors)); // 错误信息
    }

    return result; // 返回结果集
}

UniValue sendrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2) // 1.参数为 1 或 2 个
        throw runtime_error( // 命令帮助反馈
            "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw transaction (serialized, hex-encoded) to local node and network.\n"
            "\nAlso see createrawtransaction and signrawtransaction calls.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"{\\\"myaddress\\\":0.01}\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"") +
            "\nSend the transaction (signed hex)\n"
            + HelpExampleCli("sendrawtransaction", "\"signedhex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawtransaction", "\"signedhex\"")
        );

    LOCK(cs_main); // 2.上锁
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL)); // 3.参数类型检查

    // parse hex string from parameter
    CTransaction tx; // 交易对象
    if (!DecodeHexTx(tx, params[0].get_str())) // 从参数解析 16 进制字符串
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    uint256 hashTx = tx.GetHash(); // 获取交易哈希

    bool fOverrideFees = false; // 交易费超额标志，默认不允许
    if (params.size() > 1)
        fOverrideFees = params[1].get_bool(); // 获取交易费超额设置

    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hashTx); // 获取该交易的修剪版本
    bool fHaveMempool = mempool.exists(hashTx); // 交易内存池中是否存在该交易
    bool fHaveChain = existingCoins && existingCoins->nHeight < 1000000000; // 交易的高度限制
    if (!fHaveMempool && !fHaveChain) { // 4.若该交易不在交易内存池中 且 超过了高度限制即没有上链
        // push to local node and sync with wallets // 推送到本地节点并同步钱包
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptToMemoryPool(mempool, state, tx, false, &fMissingInputs, false, !fOverrideFees)) { // 放入交易内存池
            if (state.IsInvalid()) { // 进行状态检测
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", state.GetRejectCode(), state.GetRejectReason()));
            } else {
                if (fMissingInputs) {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }
                throw JSONRPCError(RPC_TRANSACTION_ERROR, state.GetRejectReason());
            }
        }
    } else if (fHaveChain) {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "transaction already in block chain");
    }
    RelayTransaction(tx); // 5.然后中继（发送）该交易

    return hashTx.GetHex(); // 6.交易哈希转换为 16 进制并返回
}
