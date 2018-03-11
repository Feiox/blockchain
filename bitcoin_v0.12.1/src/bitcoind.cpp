// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "clientversion.h"
#include "rpcserver.h"
#include "init.h"
#include "noui.h"
#include "scheduler.h"
#include "util.h"
#include "httpserver.h"
#include "httprpc.h"
#include "rpcserver.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#include <stdio.h>

/* Introduction text for doxygen: */

/*! \mainpage Developer documentation
 *
 * \section intro_sec Introduction
 *
 * This is the developer documentation of the reference client for an experimental new digital currency called Bitcoin (https://www.bitcoin.org/),
 * which enables instant payments to anyone, anywhere in the world. Bitcoin uses peer-to-peer technology to operate
 * with no central authority: managing transactions and issuing money are carried out collectively by the network.
 *
 * The software is a community-driven open source project, released under the MIT license.
 *
 * \section Navigation
 * Use the buttons <code>Namespaces</code>, <code>Classes</code> or <code>Files</code> at the top of the page to start navigating the code.
 */

static bool fDaemon;

void WaitForShutdown(boost::thread_group* threadGroup)
{
    bool fShutdown = ShutdownRequested();
    // Tell the main threads to shutdown.
    while (!fShutdown) // ѭ���ȴ��ر�����
    {
        MilliSleep(200); // ˯�� 200 ����
        fShutdown = ShutdownRequested();
    }
    if (threadGroup)
    {
        Interrupt(*threadGroup);
        threadGroup->join_all();
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// Start
//
bool AppInit(int argc, char* argv[]) // 3.0.Ӧ�ó����ʼ��
{
    boost::thread_group threadGroup; // ���߳�����󣬹�����̣߳����ɸ��ƺ��ƶ�
    CScheduler scheduler;

    bool fRet = false; // ������־�������ж�Ӧ�ó�������״̬

    //
    // Parameters
    //
    // If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
    ParseParameters(argc, argv); // 3.1.���������У�����̨���룩����

    // Process help and version before taking care about datadir
    if (mapArgs.count("-?") || mapArgs.count("-h") ||  mapArgs.count("-help") || mapArgs.count("-version")) // 3.2.�汾�Ͱ�����Ϣ
    {
        std::string strUsage = _("Bitcoin Core Daemon") + " " + _("version") + " " + FormatFullVersion() + "\n";

        if (mapArgs.count("-version"))
        {
            strUsage += LicenseInfo(); // �汾���֤��Ϣ
        }
        else
        {
            strUsage += "\n" + _("Usage:") + "\n" +
                  "  bitcoind [options]                     " + _("Start Bitcoin Core Daemon") + "\n";

            strUsage += "\n" + HelpMessage(HMM_BITCOIND); // ������Ϣ
        }

        fprintf(stdout, "%s", strUsage.c_str());
        return false;
    }

    try
    {
        if (!boost::filesystem::is_directory(GetDataDir(false))) //3.3.����Ŀ¼���Ȼ�ȡ������������ Ĭ��/ָ�� ���ִ���
        {
            fprintf(stderr, "Error: Specified data directory \"%s\" does not exist.\n", mapArgs["-datadir"].c_str());
            return false;
        }
        try
        {
            ReadConfigFile(mapArgs, mapMultiArgs); // 3.4.��ȡ�����ļ�
        } catch (const std::exception& e) {
            fprintf(stderr,"Error reading configuration file: %s\n", e.what());
            return false;
        }
        // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
        try {
            SelectParams(ChainNameFromCommandLine()); // 3.5.ѡ�������������磩���������������������ʱ������
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            return false;
        }

        // Command-line RPC
        bool fCommandLine = false;
        for (int i = 1; i < argc; i++) // 3.6.���ÿ�������в����Ƿ���'-'��'/'��ͷ
            if (!IsSwitchChar(argv[i][0]) && !boost::algorithm::istarts_with(argv[i], "bitcoin:"))
                fCommandLine = true;

        if (fCommandLine)
        {
            fprintf(stderr, "Error: There is no RPC client functionality in bitcoind anymore. Use the bitcoin-cli utility instead.\n");
            exit(1);
        }
#ifndef WIN32
        fDaemon = GetBoolArg("-daemon", false); // 3.7.Linux �¸������ú�̨����Ĭ�Ϲر�
        if (fDaemon)
        {
            fprintf(stdout, "Bitcoin server starting\n");

            // Daemonize
            pid_t pid = fork();
            if (pid < 0)
            {
                fprintf(stderr, "Error: fork() returned %d errno %d\n", pid, errno);
                return false;
            }
            if (pid > 0) // Parent process, pid is child process id
            {
                return true;
            }
            // Child process falls through to rest of initialization

            pid_t sid = setsid();
            if (sid < 0)
                fprintf(stderr, "Error: setsid() returned %d errno %d\n", sid, errno);
        }
#endif
        SoftSetBoolArg("-server", true); //3.8.�������ã�Ĭ�Ͽ�������������

        // Set this early so that parameter interactions go to console
        InitLogging(); // 3.9.��ʼ����־��¼��Ĭ������� debug.log
        InitParameterInteraction(); // 3.10.��ʼ������������˵�����ֲ��������÷���
        fRet = AppInit2(threadGroup, scheduler); // 3.11.Ӧ�ó����ʼ�� 2��������ڣ�
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInit()");
    } catch (...) {
        PrintExceptionContinue(NULL, "AppInit()");
    }

    if (!fRet) // 3.12.����������־������Ӧ����
    {
        Interrupt(threadGroup); // ����ʧ��
        // threadGroup.join_all(); was left out intentionally here, because we didn't re-test all of
        // the startup-failure cases to make sure they don't result in a hang due to some
        // thread-blocking-waiting-for-another-thread-during-startup case
    } else {
        WaitForShutdown(&threadGroup); // �����ɹ���ѭ���ȴ��ر�
    }
    Shutdown(); // 3.13.�ر�

    return fRet;
}

int main(int argc, char* argv[]) // 0.�������
{
    SetupEnvironment(); // 1.���ó������л��������ػ�����

    // Connect bitcoind signal handlers
    noui_connect(); // 2.�� UI ���ӣ������źŴ�����

    return (AppInit(argc, argv) ? 0 : 1); // 3.Ӧ�ó����ʼ������ʼ��������
}
