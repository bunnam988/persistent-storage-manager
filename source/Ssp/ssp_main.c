/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2015 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/**********************************************************************
   Copyright [2014] [Cisco Systems, Inc.]
 
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
 
       http://www.apache.org/licenses/LICENSE-2.0
 
   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
**********************************************************************/


#ifdef __GNUC__
#if (!defined _BUILD_ANDROID) && (!defined _NO_EXECINFO_H_)
#include <execinfo.h>
#endif
#endif

#include <sys/types.h>
#include <sys/ipc.h>
#include <semaphore.h>
#include <fcntl.h>
#ifdef _BUILD_ANDROID
#include <linux/msg.h>
#else
#include <sys/msg.h>
#endif

#include "ssp_global.h"
#include "safec_lib_common.h"
#include "secure_wrapper.h"
#ifdef ENABLE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#ifdef INCLUDE_BREAKPAD
#include "breakpad_wrapper.h"
#endif
#include "syscfg/syscfg.h"
#include "cap.h"

#define DEBUG_INI_NAME "/etc/debug.ini"
int GetLogInfo(ANSC_HANDLE bus_handle, char *Subsytem, char *pParameterName);
BOOL                                bEngaged          = FALSE;
void                               *bus_handle        = NULL;
char                                g_Subsystem[32]   = {0};
BOOL                                g_bLogEnable      = FALSE;
extern char*                        pComponentName;
static cap_user                     appcaps;
#ifdef USE_PLATFORM_SPECIFIC_HAL
PSM_CFM_INTERFACE                   cfm_ifo;
#endif

    sem_t *sem;


static void _print_stack_backtrace(void)
{
#ifdef __GNUC__
#if (!defined _BUILD_ANDROID) && (!defined _NO_EXECINFO_H_)
	void* tracePtrs[100];
	char** funcNames = NULL;
	int i, count = 0;

        int fd;
        const char* path = "/nvram/psmssp_backtrace";
        fd = open(path, O_RDWR | O_CREAT, 0777);
        if (fd < 0)
        {
            fprintf(stderr, "failed to open backtrace file: %s", path);
            return;
        }
 
	count = backtrace( tracePtrs, 100 );
	backtrace_symbols_fd( tracePtrs, count, fd );
        close(fd);

	funcNames = backtrace_symbols( tracePtrs, count );

	if ( funcNames ) {
		// Print the stack trace
		for( i = 0; i < count; i++ )
		   printf("%s\n", funcNames[i] );

		// Free the string pointers
		free( funcNames );
	}
#endif
#endif
}

static void daemonize(void) {
#ifndef  _DEBUG
	int fd;
#endif

	/* initialize semaphores for shared processes */
	sem = sem_open ("pSemPsm", O_CREAT | O_EXCL, 0644, 0);
	if(SEM_FAILED == sem)
	{
	       AnscTrace("Failed to create semaphore %d - %s\n", errno, strerror(errno));
	       _exit(1);
	}
	/* name of semaphore is "pSemPsm", semaphore is reached using this name */
	sem_unlink ("pSemPsm");
	/* unlink prevents the semaphore existing forever */
	/* if a crash occurs during the execution         */
	AnscTrace("Semaphore initialization Done!!\n");

	switch (fork()) {
	case 0:
		break;
	case -1:
		// Error
		AnscTrace("Error daemonizing (fork)! %d - %s\n", errno, strerror(
				errno));
		exit(0);
		break;
	default:
		sem_wait (sem);
		sem_close (sem);
		_exit(0);
	}

	if (setsid() < 	0) {
		AnscTrace("Error demonizing (setsid)! %d - %s\n", errno, strerror(errno));
		exit(0);
	}

#ifndef  _DEBUG

	fd = open("/dev/null", O_RDONLY);
	if (fd != 0) {
		dup2(fd, 0);
		close(fd);
	}
	fd = open("/dev/null", O_WRONLY);
	if (fd != 1) {
		dup2(fd, 1);
		close(fd);
	}
	fd = open("/dev/null", O_WRONLY);
	if (fd != 2) {
		dup2(fd, 2);
		close(fd);
	}
#endif
}

void sig_handler(int sig)
{
	CcspTraceInfo((" inside sig_handler\n"));
    if ( sig == SIGINT ) {
    	signal(SIGINT, sig_handler); /* reset it to this function */
    	CcspTraceError(("SIGINT received, exiting!\n"));
#if  defined(_DEBUG)
    	_print_stack_backtrace();
#endif
	exit(0);
    }
    else if ( sig == SIGUSR1 ) {
    	signal(SIGUSR1, sig_handler); /* reset it to this function */
    	CcspTraceInfo(("SIGUSR1 received!\n"));
    }
    else if ( sig == SIGUSR2 ) {
    	CcspTraceInfo(("SIGUSR2 received!\n"));
        if ( bEngaged )
        {
            bEngaged = FALSE;

    	    CcspTraceError(("Exit!\n"));

            exit(0);
        }
    }
    else if ( sig == SIGCHLD ) {
    	signal(SIGCHLD, sig_handler); /* reset it to this function */
    	CcspTraceInfo(("SIGCHLD received!\n"));
    }
    else if ( sig == SIGPIPE ) {
    	signal(SIGPIPE, sig_handler); /* reset it to this function */
    	CcspTraceInfo(("SIGPIPE received!\n"));
    }
	else if ( sig == SIGALRM ) {

    	signal(SIGALRM, sig_handler); /* reset it to this function */
    	CcspTraceInfo(("SIGALRM received!\n"));
	}
    else if (sig == SIGTERM ) {
        /* When PSM is terminated, make sure to save the config to flash before exiting so settings aren't lost */
        CcspTraceInfo(("SIGTERM make sure to save the config to flash before exiting so settings aren't lost!\n"));
		exit(0);
    }
    else {
    	/* get stack trace first */
    	_print_stack_backtrace();
    	CcspTraceError(("Signal %d received, exiting!\n", sig));
    	exit(0);
    }
	CcspTraceInfo((" sig_handler exit\n"));
}

#ifndef INCLUDE_BREAKPAD
static int is_core_dump_opened(void)
{
    FILE *fp;
    char path[256];
    char line[1024];
    char *start, *tok, *sp;
#define TITLE   "Max core file size"

    snprintf(path, sizeof(path), "/proc/%d/limits", getpid());
    if ((fp = fopen(path, "rb")) == NULL)
        return 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if ((start = strstr(line, TITLE)) == NULL)
            continue;

        start += strlen(TITLE);
        if ((tok = strtok_r(start, " \t\r\n", &sp)) == NULL)
            break;

        fclose(fp);

       return (tok[0] == '0' && tok[1] == '\0') ? 0 : 1;
    }

    fclose(fp);
    return 0;
}
#endif

syscfg_lmdb_t *g_lmdb_ctx = NULL;

int main(int argc, char* argv[])
{
    int                             cmdChar            = 0;
    BOOL                            bRunAsDaemon       = TRUE;
    int                             idx                = 0;
    FILE                           *fd                 = NULL;
    char                            cmd[64]            = {0};
    errno_t                         rc                 = -1;
    int                             ind                = -1;
    int                             ret                = 0;
    bool                            blocklist_ret     = false;

    CcspTraceInfo((" inside main\n"));
    // Buffer characters till newline for stdout and stderr
    setlinebuf(stdout);
    setlinebuf(stderr);

    pComponentName = CCSP_DBUS_PSM;
#ifdef FEATURE_SUPPORT_RDKLOG
    RDK_LOGGER_INIT();
#endif

#if defined(_DEBUG) || defined(_COSA_SIM_)
    AnscSetTraceLevel(CCSP_TRACE_LEVEL_INFO);
#endif


    for (idx = 1; idx < argc; idx++)
    {
	rc = strcmp_s("-subsys", strlen("-subsys"), argv[idx], &ind);
        ERR_CHK(rc);
	if((rc == EOK) && (ind == 0))
	{
	    if ((idx+1) < argc)
            {
                rc = strcpy_s(g_Subsystem, sizeof(g_Subsystem), argv[idx+1]);
	        if(rc != EOK)
	        {
		    ERR_CHK(rc);
		    return 1;
	        }
            }
            else
            {
               CcspTraceInfo(("Argument missing after -subsys\n"));
            }
	}
	else
	{
	    rc = strcmp_s("-c", strlen("-c"), argv[idx], &ind);
            ERR_CHK(rc);
	    if((rc == EOK) && (ind == 0))
	    {
		 bRunAsDaemon = FALSE;
            }
	}
     }
  
    appcaps.caps = NULL;
    appcaps.user_name = NULL;
    init_capability();
    drop_root_caps(&appcaps);
    CcspTraceInfo(("Dropping root privileges\n"));

    if ( bRunAsDaemon )
    	daemonize();

    fd = fopen("/var/tmp/PsmSsp.pid", "w+");
    if ( !fd )
    {
        CcspTraceWarning(("Create /var/tmp/PsmSsp.pid error. \n"));
        return 1;
    }
    else
    {
        sprintf(cmd, "%d", getpid());
        fputs(cmd, fd);
        fclose(fd);
    }

    /* Regardless of whether using breakpad, core dumps, etc, we always need to perform cleanup when the process it terminated */
    signal(SIGTERM, sig_handler);

#ifdef INCLUDE_BREAKPAD
    breakpad_ExceptionHandler();
    signal(SIGUSR1, sig_handler);
#else
    if (is_core_dump_opened())
    {
        signal(SIGUSR1, sig_handler);
        CcspTraceWarning(("Core dump is opened, do not catch signal\n"));
    }
    else
    {
        CcspTraceWarning(("Core dump is NOT opened, backtrace if possible\n"));
        signal(SIGINT, sig_handler);
        /*signal(SIGCHLD, sig_handler);*/
        signal(SIGUSR1, sig_handler);
        signal(SIGUSR2, sig_handler);

        signal(SIGSEGV, sig_handler);
        signal(SIGBUS, sig_handler);
        signal(SIGKILL, sig_handler);
        signal(SIGFPE, sig_handler);
        signal(SIGILL, sig_handler);
        signal(SIGQUIT, sig_handler);
        signal(SIGHUP, sig_handler);
        signal(SIGPIPE, sig_handler);
		signal(SIGALRM, sig_handler);
    }

#endif
    gather_info();

    ret = cmd_dispatch('e');
    if(ret != 0){
        return 1;
    }

    // LMDB initialization (after PSM DB init)
    int lmdb_rc = syscfg_lmdb_open(&g_lmdb_ctx, LMDB_PERSIST_DIR, MAPSIZE, 0); // Use defaults
    if (lmdb_rc != 0) {
        CcspTraceError(("Failed to initialize LMDB: %d\n", lmdb_rc));
        return 1;
    }
    CcspTraceInfo(("LMDB initialized successfully\n"));

    creat("/tmp/psm_initialized", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if(!blocklist_ret){
        update_process_caps(&appcaps);
        read_capability(&appcaps);
        CcspTraceInfo(("CAP_DAC_OVERRIDE removed\n"));
    }
    if ( bRunAsDaemon ) {
		sem_post (sem);
		sem_close(sem);
		while (1)
			sleep(30);
    }
    else {
        while ( cmdChar != 'q' )
        {
            cmdChar = getchar();
            if (cmdChar < 0) 
            {
                sleep(30);
            }
            else 
            {
                ret = cmd_dispatch(cmdChar);
                if(ret != 0)
                    return 1;
            }
        }
    }

    if ( bEngaged )
    {
        bEngaged = FALSE;
    }

    // Cleanup LMDB context
    if (g_lmdb_ctx) {
        syscfg_lmdb_close(g_lmdb_ctx);
        g_lmdb_ctx = NULL;
    }

    return 0;
}


int  cmd_dispatch(int  command)
{
    int ret = 0;
    CcspTraceInfo((" inside cmd_dispatch\n"));
    
    switch ( command )
    {
        case    'e' :

				if (!bEngaged)
				{
					CcspTraceInfo((" inside case 'e' !bEngaged\n"));

					/* Start buses directly (no PsmSysRegistry) */
					ret = PsmDbusInit();
					if (ret != 0)
					{
						return -1;
					}

					PsmRbusInit();

					bEngaged = TRUE;

					CcspTraceWarning(("RDKB_SYSTEM_BOOT_UP_LOG : PSM started ...\n"));

					#if !defined(INTEL_PUMA7) && !defined(_COSA_BCM_MIPS_) && !defined(_COSA_BCM_ARM_)  && !defined(_COSA_QCA_ARM_)
							v_secure_system("sysevent set bring-lan up");
					#endif
				}
				else
				{
					CcspTraceError(("RDKB_SYSTEM_BOOT_UP_LOG : Create PSM Failed ...\n"));
				}

                break;

        case    'c' :

                if ( bEngaged )
                {
                    CcspTraceInfo((" inside case 'c' bEngaged\n"));
                    CcspTraceWarning(("RDKB_SYSTEM_BOOT_UP_LOG : PSM is being unloaded ...\n"));

                    if ( bus_handle != NULL )
                    {
                        CCSP_Message_Bus_Exit(bus_handle);
                    }

                    bEngaged = FALSE;

                    CcspTraceInfo(("PSM has been unloaded.\n"));
                }

                break;

        default :

                break;
    }
	CcspTraceInfo((" cmd_dispatch exit\n"));
	return  0;
}


int  gather_info()
{
    AnscTrace("\n\n");
    AnscTrace("        ***************************************************************\n");
    AnscTrace("        ***                                                         ***\n");
    AnscTrace("        ***            PSM Testing App - Simulation                 ***\n");
    AnscTrace("        ***           Common Component Service Platform             ***\n");
    AnscTrace("        ***                                                         ***\n");
    AnscTrace("        ***          Copyright 2014 Cisco Systems, Inc.             ***\n");
    AnscTrace("        ***       Licensed under the Apache License, Version 2.0    ***\n");
    AnscTrace("        ***                                                         ***\n");
    AnscTrace("        ***************************************************************\n");
    AnscTrace("\n\n");

    return  0;
}
