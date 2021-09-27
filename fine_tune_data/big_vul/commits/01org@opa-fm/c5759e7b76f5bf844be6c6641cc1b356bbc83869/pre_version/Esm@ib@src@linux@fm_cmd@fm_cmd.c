/* BEGIN_ICS_COPYRIGHT5 ****************************************

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 * ** END_ICS_COPYRIGHT5   ****************************************/

/*
 * FILE NAME
 *    sm_diag.c
 *
 * DESCRIPTION
 *    FMI sm_diag utility used to control and diagnose a local SM instance
 *
 * RESPONSIBLE ENGINEER
 *    John Seraphin
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include "hsm_config_client_api.h"
#include "hsm_config_client_data.h"

extern int   getopt(int, char *const *, const char *);

typedef struct command_s{
	char *name;
	int (*cmdPtr)(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
	fm_mgr_type_t mgr;
	char *desc;
}command_t;

/* function prototypes */
int mgr_force_sweep(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_restore_priority(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_broadcast_xml_config(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_get_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_reset_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int pm_get_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int pm_reset_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_state_dump(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int pm_restore_priority(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int mgr_log_level(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int mgr_log_mode(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int mgr_log_mask(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_perf_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sa_perf_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int mgr_rmpp_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int mgr_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_force_rebalance_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_adaptive_routing(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_start(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_fast_mode_start(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_stop(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_inject_packets(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_inject_at_node(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_inject_packets_each_sweep(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_path_length(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_show_loop_paths(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_min_isl_redundancy(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_show_switch_lfts(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_show_loop_topology(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_show_config(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_looptest_fast(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_force_attribute_rewrite(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_skip_attr_write(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_pause_sweeps(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);
int sm_resume_sweeps(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]);

int             optind = 1;     /* index into parent argv vector */
char           *optarg;         /* argument associated with option */

static command_t commandList[] = {
	{"smForceSweep", mgr_force_sweep, FM_MGR_SM, "Make the SM sweep now"},
	{"smRestorePriority", sm_restore_priority, FM_MGR_SM, "Restore the normal priority of the SM (if it is\n                           currently elevated)"},
	{"smShowCounters", sm_get_counters, FM_MGR_SM, "Get statistics and performance counters from the SM"},
	{"smResetCounters",sm_reset_counters, FM_MGR_SM, "Reset SM statistics and performace counters"},
	{"smStateDump",sm_state_dump, FM_MGR_SM, "Dump Internal SM state into directory specified"},
	{"smLogLevel", mgr_log_level, FM_MGR_SM, "Set the SM logging level (1=WARN+, 2=INFINI_INFO+,\n                           3=INFO+, 4=VERBOSE+, 5=DEBUG2+, 6=DEBUG3+, 7=TRACE+)"},
	{"smLogMode", mgr_log_mode, FM_MGR_SM, "Set the SM log mode flags (0/1 1=downgrade\n                           non-actionable, 0/2 2=logfile only)"},
	{"smLogMask", mgr_log_mask, FM_MGR_SM, "Set the SM log mask for a specific subsystem to the\n                           value given see /etc/sysconfig/opafm.xml-sample\n                           for a list of subsystems and mask bit meanings"},
	{"smPerfDebug", sm_perf_debug_toggle, FM_MGR_SM, "Toggle performance debug output for SM"},
	{"saPerfDebug", sa_perf_debug_toggle, FM_MGR_SM, "Toggle performance debug output for SA"},
	{"saRmppDebug", mgr_rmpp_debug_toggle, FM_MGR_SM, "Toggle Rmpp debug output for SA"},
	{"pmRestorePriority", pm_restore_priority, FM_MGR_PM, "No longer supported, use smRestorePriority"},
	{"pmLogLevel", mgr_log_level, FM_MGR_PM, "No longer supported, use smLogLevel"},
	{"pmLogMode", mgr_log_mode, FM_MGR_PM, "No longer supported, use smLogMode"},
	{"pmLogMask", mgr_log_mask, FM_MGR_PM, "No longer supported, use smLogMask"},
	// these commands can be issued direct to PM without issue
	{"pmShowCounters", pm_get_counters, FM_MGR_PM, "Get statistics and performance counters about the PM"},
	{"pmResetCounters",pm_reset_counters, FM_MGR_PM, "Reset statistics and performace counters about the PM"},
	{"pmDebug", mgr_debug_toggle, FM_MGR_PM, "Toggle debug output for PM"},
	{"pmRmppDebug", mgr_rmpp_debug_toggle, FM_MGR_PM, "Toggle Rmpp debug output for PM"},
	{"feLogLevel", mgr_log_level, FM_MGR_FE, "Set the FE logging level (1=WARN+, 2=INFINI_INFO+,\n                           3=INFO+, 4=VERBOSE+, 5=DEBUG2+, 6=DEBUG3+, 7=TRACE+)"},
	{"feLogMode", mgr_log_mode, FM_MGR_FE, "Set the FE log mode flags (0/1 1=downgrade\n                           non-actionable, 0/2 2=logfile only)"},
	{"feLogMask", mgr_log_mask, FM_MGR_FE, "Set the FE log mask for a specific subsystem to the\n                           value given see /etc/sysconfig/opafm.xml-sample\n                           for a list of subsystems and mask bit meanings"},
	{"feDebug", mgr_debug_toggle, FM_MGR_FE, "Toggle debug output for FE"},
	{"feRmppDebug", mgr_rmpp_debug_toggle, FM_MGR_FE, "Toggle Rmpp debug output for FE"},
	{"smLooptestStart", sm_looptest_start, FM_MGR_SM, "START loop test in normal mode - specify the number of 256 byte packets\n                           (default=0)"},
	{"smLooptestFastModeStart", sm_looptest_fast_mode_start, FM_MGR_SM, "START loop test in fast mode - specify the number of 256 byte packets\n                           (default=4)"},
	{"smLooptestStop", sm_looptest_stop, FM_MGR_SM, "STOP the loop test (puts switch LFTs back to normal)"},
	//{"smLooptestFastMode", sm_looptest_fast, FM_MGR_SM, "1 to turn loop test fast mode ON, 0 to turn OFF"},
	{"smLooptestInjectPackets", sm_looptest_inject_packets, FM_MGR_SM, "Enter numPkts to send to all switch loops\n                           (default=1)"},
	{"smLooptestInjectAtNode", sm_looptest_inject_at_node, FM_MGR_SM, "Enter the switch node index to inject loop packets\n                           (default=0)"},
	{"smLooptestInjectEachSweep", sm_looptest_inject_packets_each_sweep, FM_MGR_SM, "1 to inject packets each sweep, 0 to stop injecting each sweep"},
	{"smLooptestPathLength", sm_looptest_path_length, FM_MGR_SM, "Sets the loop path length 2-4\n                           (default=3)"},
	{"smLooptestMinISLRedundancy", sm_looptest_min_isl_redundancy, FM_MGR_SM, "Sets the minimum number of loops in which to include each ISL\n                           (default=4)"},
	{"smLooptestShowLoopPaths",sm_looptest_show_loop_paths, FM_MGR_SM, "Displays the loop paths given node index or all loop paths\n                           (default=all)"},
	{"smLooptestShowSwitchLft",sm_looptest_show_switch_lfts, FM_MGR_SM, "Displays a switch LFT given node index or all switches LFTs\n                           (default=all)"},
	{"smLooptestShowTopology",sm_looptest_show_loop_topology, FM_MGR_SM, "Displays the topology for the SM Loop Test"},
	{"smLooptestShowConfig",sm_looptest_show_config, FM_MGR_SM, "Displays the current active loop configuration"},
	{"smForceRebalance", sm_force_rebalance_toggle, FM_MGR_SM, "Toggle Force Rebalance setting for SM"},
	{"smAdaptiveRouting", sm_adaptive_routing, FM_MGR_SM, "Displays or modifies Adaptive Routing setting for SM. Display (no arg), Disable=0, Enable=1"},
	{"smForceAttributeRewrite", sm_force_attribute_rewrite, FM_MGR_SM, "Set rewriting of all attributes upon resweeping. Disable=0, Enable=1"},
	{"smSkipAttrWrite", sm_skip_attr_write, FM_MGR_SM, "Bitmask of attributes to be skipped(not written) during sweeps (-help for list)"},
	{"smPauseSweeps", sm_pause_sweeps, FM_MGR_SM, "Pause SM sweeps"},
	{"smResumeSweeps", sm_resume_sweeps, FM_MGR_SM, "Resume SM sweeps"},
// JPW - may implement in future as part of the maint mode feature
//	{"smBroadcastConfig", sm_broadcast_xml_config, FM_MGR_SM, "Broadcast the XML configuration file to STANDBY and INACTIVE SM's"},
};

static int commandListLen = (sizeof(commandList)/sizeof(commandList[0]));


void
usage(char *cmd)
{
	int i;

	fprintf(stderr, "USAGE: %s", cmd);
	fprintf(stderr, " [OPTIONS] COMMAND [COMMAND ARGS]\n\n");

	fprintf(stderr,
			"OPTIONS:\n");
	fprintf(stderr, "  -i <VAL>\t\tinstance to connect to (0 - default)\n");
	//fprintf(stderr, "  -d <VAL>\t\tdestination ip address or hostname. (Use to connect to a remote instance)\n");

	fprintf(stderr,
			"COMMANDS:\n");
	for(i=0;i<commandListLen;i++){
		fprintf(stderr, "  %-21s %s\n",commandList[i].name,commandList[i].desc);
	}
	fflush(stderr);

}

int mgr_force_sweep(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_FORCE_SWEEP, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "mgr_force_sweep: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("mgr_force_sweep: Successfully sent Force Sweep control to local mgr instance\n");
    }
	return 0;
}

int sm_broadcast_xml_config(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_BROADCAST_XML_CONFIG, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_broadcast_xml_config: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("sm_broadcast_xml_config: Successfully sent XML broadcast config command to local mgr instance\n");
    }
	return 0;
}

int sm_restore_priority(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_RESTORE_PRIORITY, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_restore_priority: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("sm_restore_priority: Successfully sent Relinquish Master control to local mgr instance\n");
    }
	return 0;
}

#define BUF_SZ 16384

int sm_get_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint8_t data[BUF_SZ];
	time_t timeNow;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_GET_COUNTERS, mgr, BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_get_counters: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		time(&timeNow);
		data[BUF_SZ-1]=0;
		printf("%35s: %s%s", "SM Counters as of", ctime(&timeNow), (char*) data);
    }
	return 0;
}

int pm_get_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint8_t data[BUF_SZ];
	time_t timeNow;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_PM_GET_COUNTERS, mgr, BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "pm_get_counters: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		time(&timeNow);
		data[BUF_SZ-1]=0;
		printf("PM Counters as of %s%s", ctime(&timeNow), (char*) data);
    }
	return 0;
}

int sm_reset_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_RESET_COUNTERS, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_reset_counters: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("sm_reset_counters: Successfully sent reset command "
		       "to the SM\n");
    }
	return 0;
}

int pm_reset_counters(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_PM_RESET_COUNTERS, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "pm_reset_counters: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("pm_reset_counters: Successfully sent reset command "
		       "to the PM\n");
    }
	return 0;
}

int sm_state_dump(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
    char dirName[256];

	if (argc == 1 && strlen(argv[0]) < 256) {
		strncpy(dirName, argv[0], sizeof(dirName));
		dirName[sizeof(dirName)-1]=0;
	} else {
		sprintf(dirName, "/tmp");
	}

	printf("Sending command to dump the SM state into the directory %s\n", dirName);

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_DUMP_STATE, mgr, 
								  strlen(dirName) + 1, dirName, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_state_dump: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent state dump command to local SM instance\n");
	}

	return 0;
}


int pm_restore_priority(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fprintf(stderr, "pmRestorePriority:\n");
	fprintf(stderr, "\tThis command is not supported any more.  The priority of the\n");
	fprintf(stderr, "\tPerformance Manager(PM) is now based on the priority of the\n");
	fprintf(stderr, "\tSubnet manager(SM).  Use the smRestorePriority command\n");
	fprintf(stderr, "\tfor restoring the priority of the SM and PM.\n");
	return 0;
}


int mgr_log_level(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint32_t 				loglevel=0;

	if (mgr == FM_MGR_PM) {
		fprintf(stderr, "pmLogLevel:\n");
		fprintf(stderr, "\tThis command is not supported any more.  The logging of the\n");
		fprintf(stderr, "\tPerformance Manager(PM) is now\n");
		fprintf(stderr, "\tbased on the logging of the Subnet manager(SM).  Use the\n");
		fprintf(stderr, "\tsmLogLevel command for changing the logging level of the\n");
		fprintf(stderr, "\tSM and PM\n");
	} else if (argc == 1) {
		loglevel = atol(argv[0]);
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_LOG_LEVEL, mgr, sizeof(loglevel), (void *)&loglevel, &ret_code)) != FM_CONF_OK)
		{
			fprintf(stderr, "mgr_log_level: Failed to retrieve data: \n"
				   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
				   res, fm_mgr_get_error_str(res),ret_code,
				   fm_mgr_get_resp_error_str(ret_code));
		} else {
			printf("mgr_log_level: Successfully sent Log Level control to local mgr instance\n");
		}
	} else {
		fprintf(stderr, "mgr_log_level: must specify the log level parameter (1 > 5): \n");
	}

	return 0;
}

int mgr_log_mode(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint32_t				logmode=0;

	if (mgr == FM_MGR_PM) {
		fprintf(stderr, "pmLogMode:\n");
		fprintf(stderr, "\tThis command is not supported any more.  The logging of the\n");
		fprintf(stderr, "\tPerformance Manager(PM) is now\n");
		fprintf(stderr, "\tbased on the logging of the Subnet manager(SM).  Use the\n");
		fprintf(stderr, "\tsmLogMode command for changing the logging level of the\n");
		fprintf(stderr, "\tSM and PM\n");
	} else if (argc == 1) {
		logmode = atol(argv[0]);
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_LOG_MODE, mgr, sizeof(logmode), (void *)&logmode, &ret_code)) != FM_CONF_OK)
		{
			fprintf(stderr, "mgr_log_mode: Failed to retrieve data: \n"
				   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
				   res, fm_mgr_get_error_str(res),ret_code,
				   fm_mgr_get_resp_error_str(ret_code));
		} else {
			printf("mgr_log_mode: Successfully sent Log Mode control to local mgr instance\n");
		}
	} else {
		fprintf(stderr, "mgr_log_mode: must specify the log mode parameter (1 > 5): \n");
	}

	return 0;
}


int mgr_log_mask(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint32_t				mask=0;
	char buf[32];			// 32 bit mask followed by subsystem name

	if (mgr == FM_MGR_PM) {
		fprintf(stderr, "pmLogMask:\n");
		fprintf(stderr, "\tThis command is not supported any more.  The logging of the\n");
		fprintf(stderr, "\tPerformance Manager(PM) is now\n");
		fprintf(stderr, "\tbased on the logging of the Subnet manager(SM).  Use the\n");
		fprintf(stderr, "\tsmLogMask command for changing the logging level of the\n");
		fprintf(stderr, "\tSM and PM\n");

	} else if (argc == 2) {
		mask = strtoul(argv[1], NULL, 0);
		//mask = hton32(mask);	// TBD - endian issues seem to be ignored here
		memcpy(buf, &mask, sizeof(mask));
		strncpy(buf+sizeof(mask), argv[0], sizeof(buf)-sizeof(mask));
		buf[sizeof(buf)-1] = '\0';
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_LOG_MASK, mgr, sizeof(buf), (void *)&buf[0], &ret_code)) != FM_CONF_OK)
		{
			fprintf(stderr, "mgr_log_mask: Failed to retrieve data: \n"
				   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
				   res, fm_mgr_get_error_str(res),ret_code,
				   fm_mgr_get_resp_error_str(ret_code));
		} else {
			printf("mgr_log_mask: Successfully sent Log Mask control to local mgr instance\n");
		}
	} else {
		fprintf(stderr, "mgr_log_mask: must specify the subsystem and mask\n");
	}

	return 0;
}


int sm_perf_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_PERF_DEBUG_TOGGLE, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_perf_debug_toggle: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent SM Perf Debug output control to local SM instance\n");
    }
	return 0;
}

int sa_perf_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SA_PERF_DEBUG_TOGGLE, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sa_perf_debug_toggle: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent SA Perf Debug output control to local SM instance\n");
    }
	return 0;
}

int mgr_rmpp_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_RMPP_DEBUG_TOGGLE, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sa_rmpp_debug_toggle: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Rmpp Debug output control to local Manager instance\n");
	}
	return 0;
}

int mgr_debug_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_DEBUG_TOGGLE, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "mgr_debug_toggle: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Debug output control to local Manager instance\n");
	}
	return 0;
}

int sm_force_rebalance_toggle(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_FORCE_REBALANCE_TOGGLE, mgr, 0, NULL, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_force_rebalance_toggle: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent SM Force Rebalance control to local SM instance\n");
    }
	return 0;
}

int sm_adaptive_routing(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint32_t				enable=0;

	if (argc == 1) {
		enable = atol(argv[0]);
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_SET_ADAPTIVE_ROUTING, mgr, sizeof(enable), (void*)&enable, &ret_code)) != FM_CONF_OK)
		{
			fprintf(stderr, "sm_adaptive_routing: Failed to retrieve data: \n"
		       	"\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       	res, fm_mgr_get_error_str(res),ret_code,
		       	fm_mgr_get_resp_error_str(ret_code));
		} else {
			printf("Successfully sent SM Adaptive Routing control to local SM instance\n");
    	}

	} else if (argc == 0) {
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_GET_ADAPTIVE_ROUTING, mgr, sizeof(enable), (void*)&enable, &ret_code)) != FM_CONF_OK)
		{
			fprintf(stderr, "sm_adaptive_routing: Failed to retrieve data: \n"
		       	"\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       	res, fm_mgr_get_error_str(res),ret_code,
		       	fm_mgr_get_resp_error_str(ret_code));
		} else {
			printf("SM Adaptive Routing is %s\n", enable ? "enabled" : "disabled");
		}
	}
	return 0;
}

int sm_pause_sweeps(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {

	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_PAUSE_SWEEPS, mgr, 0, NULL, &ret_code)) != FM_CONF_OK) {
		fprintf(stderr, "sm_pause_sweeps: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("sm_pause_sweeps: Successfully sent Pause SM Sweeps command to local mgr instance\n");
    }

	return 0;
}

int sm_resume_sweeps(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {

	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_RESUME_SWEEPS, mgr, 0, NULL, &ret_code)) != FM_CONF_OK) {
		fprintf(stderr, "sm_pause_sweeps: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("sm_resume_sweeps: Successfully sent Resume SM Sweeps command to local mgr instance\n");
    }

	return 0;
}

int sm_looptest_start(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						numpkts=0;
	uint8_t 				data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		numpkts = atol(argv[0]);
		if (numpkts < 0 || numpkts > 10) {
			printf("Error: number of packets must be from 0 to 10\n");
			return 0;
		}
	}
	*(int*)data = numpkts;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_START, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_start: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test START control (%d inject packets) to local SM instance\n", numpkts);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_fast_mode_start(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						numpkts=4;
	uint8_t 				data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		numpkts = atol(argv[0]);
		if (numpkts < 0 || numpkts > 10) {
			printf("Error: number of packets must be from 0 to 10\n");
			return 0;
		}
	}
	*(int*)data = numpkts;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_FAST_MODE_START, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_fast_mode_start: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Fast Mode START control (%d inject packets) to local SM instance\n", numpkts);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_stop(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint8_t data[BUF_SZ];

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_STOP, mgr, BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_stop: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test STOP control to local SM instance\n");
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
    }
	return 0;
}

int sm_looptest_inject_packets(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						numpkts=1;
	uint8_t                 data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		numpkts = atol(argv[0]);
		if (numpkts < 1 || numpkts > 10) {
			printf("Error: number of packets must be from 1 to 10\n");
			return 0;
		}
	}
	*(int*)data = numpkts;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_INJECT_PACKETS, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_inject_packets: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Inject %d Packets control to local SM instance\n", numpkts);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_inject_at_node(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						nodeidx=0;
	uint8_t                 data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		nodeidx = atol(argv[0]);
	}
	*(int*)data = nodeidx;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_INJECT_ATNODE, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_inject_at_node: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Inject Packets at Node index %d control to local SM instance\n", 
			   nodeidx);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_inject_packets_each_sweep(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						inject=1;
	uint8_t                 data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		inject = atol(argv[0]);
	}
	*(int*)data = inject;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_INJECT_EACH_SWEEP, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_inject_packets_each_sweep: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Inject Packet Each Sweep %d control to local SM instance\n", inject);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_path_length(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						plen=3;
	uint8_t                 data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}

	if (argc == 1) {
		plen = atol(argv[0]);
		if (plen < 2 || plen > 4) {
			printf("Error: length must be 2-4\n");
			return 0;
		}
	}
	*(int*)data = plen;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_PATH_LEN, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_path_length: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Path Length set to %d control to local SM instance\n", plen);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_show_loop_paths(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t		res;
	fm_msg_ret_code_t			ret_code;
	fm_config_interation_data_t	interationData;
	uint8_t 					data[BUF_SZ];
	int							index = -1;
	int							start;

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		index = atol(argv[0]);
	}
	memset(&interationData, 0, sizeof(fm_config_interation_data_t));
	interationData.start = start = 1;
	interationData.index = index;
	while (!interationData.done) {
		memcpy(data, &interationData, sizeof(fm_config_interation_data_t));
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_SHOW_PATHS, mgr,
						BUF_SZ, data, &ret_code)) != FM_CONF_OK) {
			fprintf(stderr, "sm_looptest_show_loop_paths: Failed to retrieve data: \n"
		   		"\tError:(%d) %s \n\tRet code:(%d) %s\n",
		   		res, fm_mgr_get_error_str(res),ret_code,
		   	fm_mgr_get_resp_error_str(ret_code));
			return 0;
		}
		if (start) {
			if (index == -1)
				printf("Successfully sent Loop Test Path show for node index (all) to local SM instance\n");
			else
				printf("Successfully sent Loop Test Path show for node index %d to local SM instance\n", index);
			start = 0;
		}
		memcpy(&interationData, data, sizeof(fm_config_interation_data_t));
		printf("%s", interationData.intermediateBuffer);
	}
	return 0;
}

int sm_looptest_min_isl_redundancy(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						plen=1;
	uint8_t                 data[BUF_SZ];

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		plen = atol(argv[0]);
	}
	*(int*)data = plen;
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_MIN_ISL_REDUNDANCY, mgr, 
								  BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_path_min_isl_redundancy: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Min ISL redundancy set to %d control to local SM instance\n", plen);
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
	}
	return 0;
}

int sm_looptest_fast(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int						plen=1;

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		plen = atol(argv[0]);
	}
	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_FAST, mgr, 
								  sizeof(plen), (void *)&plen, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_fast: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test Fast Mode %d control to local SM instance\n", plen);
	}
	return 0;
}

int sm_looptest_show_switch_lfts(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t		res;
	fm_msg_ret_code_t			ret_code;
	uint8_t 					data[BUF_SZ];
	int							index=-1;
	fm_config_interation_data_t	interationData;
	int							start;

	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		index = atol(argv[0]);
	}
	memset(&interationData, 0, sizeof(fm_config_interation_data_t));
	interationData.start = start = 1;
	interationData.index = index;
	while (!interationData.done) {
		memcpy(data, &interationData, sizeof(fm_config_interation_data_t));
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_SHOW_LFTS, mgr,
						BUF_SZ, data, &ret_code)) != FM_CONF_OK) {
			fprintf(stderr, "sm_looptest_show_switch_lfts: Failed to retrieve data: \n"
		   		"\tError:(%d) %s \n\tRet code:(%d) %s\n",
		   		res, fm_mgr_get_error_str(res),ret_code,
		   	fm_mgr_get_resp_error_str(ret_code));
			return 0;
		}
		if (start) {
			start = 0;
			if (index == -1)
				printf("Successfully sent Loop Test LFT show for node index (all) to local SM instance\n");
			else
				printf("Successfully sent Loop Test LFT show for node index %d to local SM instance\n", index);
		}
		memcpy(&interationData, data, sizeof(fm_config_interation_data_t));
		printf("%s", interationData.intermediateBuffer);
	}
	return 0;
}

int sm_looptest_show_loop_topology(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t		res;
	fm_msg_ret_code_t			ret_code;
	uint8_t 					data[BUF_SZ];
	fm_config_interation_data_t	interationData;
	int							start;

	memset(&interationData, 0, sizeof(fm_config_interation_data_t));
	interationData.start = start = 1;
	interationData.index = 0;
	while (!interationData.done) {
		memcpy(data, &interationData, sizeof(fm_config_interation_data_t));
		if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_SHOW_TOPO, mgr,
						BUF_SZ, data, &ret_code)) != FM_CONF_OK) {
			fprintf(stderr, "sm_looptest_show_loop_topology: Failed to retrieve data: \n"
		   		"\tError:(%d) %s \n\tRet code:(%d) %s\n",
		   		res, fm_mgr_get_error_str(res),ret_code,
		   	fm_mgr_get_resp_error_str(ret_code));
			return 0;
		}
		if (start) {
			start = 0;
			printf("Successfully sent Loop Test topology show to local SM instance\n");
		}
		memcpy(&interationData, data, sizeof(fm_config_interation_data_t));
		printf("%s", interationData.intermediateBuffer);
	}
	return 0;
}

int sm_looptest_show_config(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	uint8_t data[BUF_SZ];

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_LOOP_TEST_SHOW_CONFIG, mgr, BUF_SZ, data, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_looptest_show_config: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent Loop Test configuration show to local SM instance\n");
		data[BUF_SZ-1]=0;
		printf("%s", (char*) data);
    }
	return 0;
}

int sm_force_attribute_rewrite(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	int attrRewrite = 0;
	
	if (argc > 1) {
		printf("Error: only 1 argument expected\n");
		return 0;
	}
	if (argc == 1) {
		attrRewrite = atol(argv[0]);
		if (attrRewrite < 0 || attrRewrite > 1) {
			printf("Error: attrRewrite must be either 0 (disable) or 1 (enable)\n");
			return 0;
		}
	}


	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_FORCE_ATTRIBUTE_REWRITE, mgr, sizeof(attrRewrite), (void*) &attrRewrite, &ret_code)) != FM_CONF_OK)
	{
		fprintf(stderr, "sm_force_attribute_rewrite: Failed to retrieve data: \n"
		       "\tError:(%d) %s \n\tRet code:(%d) %s\n",
		       res, fm_mgr_get_error_str(res),ret_code,
		       fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent set to %d of force attribute rewriting to local SM instance\n", attrRewrite);
	}
	return 0;
}

int sm_skip_attr_write(p_fm_config_conx_hdlt hdl, fm_mgr_type_t mgr, int argc, char *argv[]) {
	fm_mgr_config_errno_t	res;
	fm_msg_ret_code_t		ret_code;
	unsigned int attrSkip = 0;

	if (argc > 1) {
		printf("Error: only 1 argument or less expected\n");
		return 0;
	}
	if ((argc==0) || ((argc==1) && (strcmp(argv[0],"-help")==0)) ) {
		printf(" SM SKIP WRITE BITMASKS...\n");
		printf("   SM_SKIP_WRITE_PORTINFO   0x00000001  (Includes Port Info)\n");
		printf("   SM_SKIP_WRITE_SMINFO     0x00000002  (Includes Sm Info)\n");
		printf("   SM_SKIP_WRITE_GUID       0x00000004  (Includes GUID Info\n");
		printf("   SM_SKIP_WRITE_SWITCHINFO 0x00000008  (Includes Switch Info\n");
		printf("   SM_SKIP_WRITE_SWITCHLTV  0x00000010  (Includes Switch LTV)\n");
		printf("   SM_SKIP_WRITE_VLARB      0x00000020  (Includes VLArb Tables/Preempt Tables)\n");
		printf("   SM_SKIP_WRITE_MAPS       0x00000040  (Includes SL::SC, SC::SL, SC::VL)\n");
		printf("   SM_SKIP_WRITE_LFT        0x00000080  (Includes LFT, MFT)\n");
		printf("   SM_SKIP_WRITE_AR         0x00000100  (Includes PG table, PG FDB)\n");
		printf("   SM_SKIP_WRITE_PKEY       0x00000200\n");
		printf("   SM_SKIP_WRITE_CONG       0x00000400  (Includes HFI / Switch congestion)\n");
		printf("   SM_SKIP_WRITE_BFRCTRL    0x00000800\n");
		printf("   SM_SKIP_WRITE_NOTICE     0x00001000\n");
		return  0;
	}

	attrSkip = strtol(argv[0],NULL,0);

	if((res = fm_mgr_simple_query(hdl, FM_ACT_GET, FM_DT_SM_SKIP_ATTRIBUTE_WRITE, mgr, sizeof(attrSkip), (void*) &attrSkip, &ret_code)) != FM_CONF_OK) {
		fprintf(stderr, "sm_skip_attr_write: Failed to retrieve data: \n"
			   "\tError:(%d) %s \n\tRet code:(%d) %s\n",
			   res, fm_mgr_get_error_str(res),ret_code,
			   fm_mgr_get_resp_error_str(ret_code));
	} else {
		printf("Successfully sent set to 0x%x of skip write to local SM instance\n", attrSkip);
	}
	return 0;
}

int main(int argc, char *argv[]) {
	p_fm_config_conx_hdlt	hdl;
	int						instance = 0;
	fm_mgr_config_errno_t	res;
	char					*rem_addr = NULL;
	char					*community = "public";
	char            		Opts[256];
    int             		arg;
	char 					*command;
	int						i;

	/* Get options at the command line (overide default values) */
    strcpy(Opts, "i:d:h-");

    while ((arg = getopt(argc, argv, Opts)) != EOF) {
        switch (arg) {
		case 'h':
		case '-':
			usage(argv[0]);
			return(0);
		case 'i':
			instance = atol(optarg);
			break;
		case 'd':
			rem_addr = optarg;
			break;
		default:
			usage(argv[0]);
			return(-1);
		}
	}

	if(optind >= argc){
        fprintf(stderr, "Command required\n");
		usage(argv[0]);
		return -1;
	}

	command = argv[optind++];
	printf("Connecting to %s FM instance %d\n", (rem_addr==NULL) ? "LOCAL":rem_addr, instance);
	if((res = fm_mgr_config_init(&hdl,instance, rem_addr, community)) != FM_CONF_OK)
	{
		fprintf(stderr, "Failed to initialize the client handle: %d\n", res);
		goto die_clean;
	}

	if((res = fm_mgr_config_connect(hdl)) != FM_CONF_OK)
	{
		fprintf(stderr, "Failed to connect: (%d) %s\n",res,fm_mgr_get_error_str(res));
		goto die_clean;
	}

	for(i=0;i<commandListLen;i++){
		if(strcmp(command,commandList[i].name) == 0){
			return commandList[i].cmdPtr(hdl, commandList[i].mgr, (argc - optind), &argv[optind]);
		}
	}

	fprintf(stderr, "Command (%s) is not valid\n",command);
	usage(argv[0]);
	res = -1;

die_clean:
	if (hdl) free(hdl);
	return res;
}
