/*

  Kjetil Matheussen 2006.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

// Only necessary with old 2.6 kernel (before jan 2006 or thereabout).
// 2.4 and newer 2.6 works fine.
#define TIMERCHECKS 0

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/types.h>

#include <pthread.h>
#include <pwd.h>

#include <sched.h>
#include <sys/mman.h>
#include <syslog.h>

#include <glibtop.h>

#include <glibtop/proclist.h>
#include <glibtop/procstate.h>
#if LIBGTOP_MAJOR_VERSION<2
#  include <glibtop/xmalloc.h>
#endif
#include <glibtop/procuid.h>
#include <glibtop/proctime.h>

#if LIBGTOP_MAJOR_VERSION<2
typedef u_int64_t ui64;
#else
typedef guint64 ui64;
#endif

#define OPTARGS_BEGIN(das_usage) {int lokke;const char *usage=das_usage;for(lokke=1;lokke<argc;lokke++){char *a=argv[lokke];if(!strcmp("--help",a)||!strcmp("-h",a)){printf(usage);return 0;
#define OPTARG(name,name2) }}else if(!strcmp(name,a)||!strcmp(name2,a)){{
#define OPTARG_GETINT() atoi(argv[++lokke])
#define OPTARG_GETFLOAT() atof(argv[++lokke])
#define OPTARG_GETSTRING() argv[++lokke]
#define OPTARG_LAST() }}else if(lokke==argc-1){lokke--;{
#define OPTARGS_ELSE() }else if(1){
#define OPTARGS_END }else{fprintf(stderr,usage);return(-1);}}}


static int increasetime=1; // Seconds between each time the SCHED_OTHER thread is increasing the counter.
static int checktime=4;    // Seconds between each time the SCHED_FIFO thread checks that the counter is increased.
static int waittime=8;     // Seconds the SCHED_FIFO thread waits before setting the processes back to SCHED_FIFO.

struct das_proclist{
  pid_t pid;
  int policy; //SCHED_OTHER, SCHED_FIFO, SHED_RR
  int priority;
  ui64 start_time; // Creation time of the process.
};
struct proclistlist{
  struct das_proclist *proclist;
  int length;
};


static int verbose=0;
int counter=0; // Make non-static in case the c compiler does a whole-program optimization. :-)

#if TIMERCHECKS
static int checkirq=0;
#endif

static int xmessage_found=1;


static void print_error(FILE *where,char *fmt, ...) {
  char temp[10000];
  va_list ap;
  va_start(ap, fmt);{
    vsnprintf (temp, 9998, fmt, ap);
  }va_end(ap);
  syslog(LOG_INFO,temp);
  if(where!=NULL)
    fprintf(where,"Das_Watchdog: %s\n",temp);
}

static ui64 get_pid_start_time(pid_t pid){
  glibtop_proc_time buf={0};
  glibtop_get_proc_time(&buf,pid);
  return buf.start_time;
}


static int get_pid_priority(pid_t pid){
  struct sched_param par;
  sched_getparam(pid,&par);
  return par.sched_priority;
}

static int set_pid_priority(pid_t pid,int policy,int priority,char *message,char *name){
  struct sched_param par={0};
  par.sched_priority=priority;
  if((sched_setscheduler(pid,policy,&par)!=0)){
    print_error(stderr,message,pid,name,strerror(errno));
    return 0;
  }
  return 1;
}


struct das_proclist *get_proclist(int *num_procs){
  int lokke=0;

  glibtop_proclist proclist_def={0};
  pid_t *proclist=glibtop_get_proclist(&proclist_def,GLIBTOP_KERN_PROC_ALL,0); //|GLIBTOP_EXCLUDE_SYSTEM,0);
  struct das_proclist *ret=calloc(sizeof(struct das_proclist),proclist_def.number);

  *num_procs=proclist_def.number;

  for(lokke=0;lokke<proclist_def.number;lokke++){
    pid_t pid=proclist[lokke];
    ret[lokke].pid=pid;
    ret[lokke].policy=sched_getscheduler(pid);
    ret[lokke].priority=get_pid_priority(pid);
    ret[lokke].start_time=get_pid_start_time(pid);
  }

#if LIBGTOP_MAJOR_VERSION<2
  glibtop_free(proclist);
#else
  g_free(proclist);
#endif

  return ret;
}

struct proclistlist *pll_create(void){
  struct proclistlist *pll=calloc(1,sizeof(struct proclistlist));
  pll->proclist=get_proclist(&pll->length);
  return pll;
}

static void pll_delete(struct proclistlist *pll){
  free(pll->proclist);
  free(pll);
}



static pid_t name2pid(char *name){
  pid_t pid=-1;
  int lokke;
  int num_procs=0;
  struct das_proclist *proclist=get_proclist(&num_procs);
    
  for(lokke=0;lokke<num_procs;lokke++){
    glibtop_proc_state state;
    glibtop_get_proc_state(&state,proclist[lokke].pid);
    if(!strcmp(state.cmd,name)){
      pid=proclist[lokke].pid;
      break;
    }
  }
  free(proclist);
  return pid;
}



static int is_a_member(int val,int *vals,int num_vals){
  int lokke;
  for(lokke=0;lokke<num_vals;lokke++)
    if(val==vals[lokke])
      return 1;
  return 0;
}



// Returns a list of users that might be the one owning the proper .Xauthority file.
static int *get_userlist(struct proclistlist *pll, int *num_users){
  int *ret=calloc(sizeof(int),pll->length);
  int lokke;

  *num_users=0;

  for(lokke=0;lokke<pll->length;lokke++){
    glibtop_proc_uid uid;
    glibtop_get_proc_uid(&uid,pll->proclist[lokke].pid);
    if( ! is_a_member(uid.uid,ret,*num_users)){ // ???
      ret[*num_users]=uid.uid;
      (*num_users)++;
    }
  }
  return ret;
}



static int gettimerpid(char *name,int cpu){
  pid_t pid;
  char temp[500];

  if(name==NULL)
    name=&temp[0];

  sprintf(name,"softirq-timer/%d",cpu);

  pid=name2pid(name);

  if(pid==-1){
    sprintf(name,"ksoftirqd/%d",cpu);
    pid=name2pid(name);
  }

  return pid;
}



#if TIMERCHECKS
static int checksoftirq2(int force,int cpu){
  char name[500];
  pid_t pid=gettimerpid(&name[0],cpu);

  if(pid==-1) return 0;


  {
    int policy=sched_getscheduler(pid);
    int priority=get_pid_priority(pid);

    if(priority<sched_get_priority_max(SCHED_FIFO)
       || policy==SCHED_OTHER
       )
      {

	if(force){
	  print_error(stdout,"Forcing %s to SCHED_FIFO priority %d",name,sched_get_priority_max(SCHED_FIFO));
	  set_pid_priority(pid,SCHED_FIFO,sched_get_priority_max(SCHED_FIFO),"Could not set %d (\"%s\") to SCHED_FIFO (%s).\n\n",name);
	  return checksoftirq2(0,cpu);
	}	  
	

	if(priority<sched_get_priority_max(SCHED_FIFO))
	  print_error(stderr,
		      "\n\nWarning. The priority of the \"%s\" process is only %d, and not %d. Unless you are using the High Res Timer,\n"
		      "the watchdog will probably not work. If you are using the High Res Timer, please continue doing so and ignore this message.\n",
		      name,
		      priority,
		      sched_get_priority_max(SCHED_FIFO)
		      );
	if(policy==SCHED_OTHER)
	  print_error(stderr,
		      "\n\nWarning The \"%s\" process is running SCHED_OTHER. Unless you are using the High Res Timer,\n"
		      "the watchdog will probably not work, and the timing on your machine is probably horrible.\n",
		      name
		      );
	
	if(checkirq){
	  print_error(stdout,"\n\nUnless you are using the High Res Timer, you need to add the \"--force\" flag to run das_watchdog reliably.\n");
	  print_error(stdout,"(Things might change though, so it could work despite all warnings above. To test the watchdog, run the \"test_rt\" program.)\n\n");
	}
	return -1;
      }
    //printf("name: -%s-\n",state.cmd);
    
    return 1;
  }
}


static int checksoftirq(int force){
  int cpu=0;

  for(;;){
    switch(checksoftirq2(force,cpu)){
    case -1:
      return -1;
    case 1:
      cpu++;
      break;
    case 0:
    default:
      return 0;
    }
  }
  return 0;
}
#endif



static char *get_pid_environ_val(pid_t pid,char *val){
  char temp[500];
  int i=0;
  int foundit=0;
  FILE *fp;

  sprintf(temp,"/proc/%d/environ",pid);

  fp=fopen(temp,"r");
  if(fp==NULL)
    return NULL;

  
  for(;;){
    temp[i]=fgetc(fp);    

    if(foundit==1 && (temp[i]==0 || temp[i]=='\0' || temp[i]==EOF)){
      char *ret;
      temp[i]=0;
      ret=malloc(strlen(temp)+10);
      sprintf(ret,"%s",temp);
      fclose(fp);
      return ret;
    }

    switch(temp[i]){
    case EOF:
      fclose(fp);
      return NULL;
    case '=':
      temp[i]=0;
      if(!strcmp(temp,val)){
	foundit=1;
      }
      i=0;
      break;
    case '\0':
      i=0;
      break;
    default:
      i++;
    }
  }
}

// Returns 1 in case a message was sent.
static int send_xmessage(char *xa_filename,char *message){
  if(access(xa_filename,R_OK)==0){
    setenv("XAUTHORITY",xa_filename,1);
    if(verbose)
      print_error(stdout,"Trying xauth file \"%s\"",xa_filename);
    if(system(message)==0)
      return 1;
  }
  return 0;
}

// Returns 1 in case a message was sent.
static int send_xmessage_using_XAUTHORITY(struct proclistlist *pll,int lokke,char *message){

  if(lokke==pll->length)
    return 0;

  {
    char *xa_filename=get_pid_environ_val(pll->proclist[lokke].pid,"XAUTHORITY");
    if(xa_filename!=NULL){
      if(send_xmessage(xa_filename,message)==1){
	free(xa_filename);
	return 1;
      }	
    }
    free(xa_filename);
  }

  return send_xmessage_using_XAUTHORITY(pll,lokke+1,message);
}

int send_xmessage_using_uids(struct proclistlist *pll, char *message){
  int num_users;
  int lokke;
  int *uids=get_userlist(pll,&num_users);
  for(lokke=0;lokke<num_users;lokke++){
    char xauthpath[5000];
    struct passwd *pass=getpwuid(uids[lokke]);
    sprintf(xauthpath,"%s/.Xauthority",pass->pw_dir);
    if(send_xmessage(xauthpath,message)==1){
      free(uids);
      return 1;
    }
  }
  
  free(uids);
  return 0;
}




static void xmessage_fork(struct proclistlist *pll){
  char message[5000];

  set_pid_priority(0,SCHED_FIFO,sched_get_priority_min(SCHED_FIFO),"Unable to set SCHED_FIFO for %d (\"%s\"). (%s)", "the xmessage fork");

  setenv("DISPLAY",":0.0",1);

  if( ! xmessage_found)
    sprintf(message,"xmessage \"WARNING! das_watchdog pauses realtime operations for %d seconds.\"",waittime);
  else
    sprintf(message,"%s \"WARNING! das_watchdog pauses realtime operations for %d seconds.\"",WHICH_XMESSAGE,waittime);

  if(send_xmessage_using_uids(pll,message)==0){
    set_pid_priority(0,SCHED_OTHER,0,"Unable to set SCHED_OTHER for %d (\"%s\"). (%s)", "the xmessage fork"); // send_xmessage_using_XAUTHRITY is too heavy to run in realtime.
    send_xmessage_using_XAUTHORITY(pll,0,message);
  }

  pll_delete(pll);
}



// The SCHED_OTHER thread.
static void *counter_func(void *arg){

  {
    set_pid_priority(0,SCHED_FIFO,sched_get_priority_min(SCHED_FIFO),"Unable to set SCHED_FIFO for %d (\"%s\"). (%s)", "the counter_func");
  }

  for(;;){
    counter++;
    if(verbose)
      print_error(stderr,"counter set to %d",counter);
    sleep(increasetime);
  }
  return NULL;
}





int main(int argc,char **argv){
  pid_t mypid=getpid();
  pthread_t counter_thread={0};
  int num_cpus=0;
  int *timerpids;
#if TIMERCHECKS
  int force=0;
#endif
  int testing=0;

  // Get timer pids
  {
    // Find number of timer processes.
    while(gettimerpid(NULL,num_cpus)!=-1)
      num_cpus++;
    timerpids=malloc(sizeof(int)*num_cpus);

    {
      int cpu=0;
      for(cpu=0;cpu<num_cpus;cpu++)
	timerpids[cpu]=gettimerpid(NULL,cpu);
    }
  }


  // Options.
  OPTARGS_BEGIN("Usage: das_watchdog [--force] [--verbose] [--checkirq] [--increasetime n] [--checktime n] [--waittime n]\n"
		"                    [ -f]     [ -v]       [ -c]        [ -it n]           [ -ct n]        [ -wt n]\n"
		"\n"
		"Additional arguments:\n"
		"[--version] or [-ve]              -> Prints out version.\n"
		"[--test]    or [-te]              -> Run a test to see if xmessage is working.\n")
    
    {
      OPTARG("--verbose","-v") verbose=1;
#if TIMERCHECKS
      OPTARG("--force","-f") force=1;
      OPTARG("--checkirq","-c") checkirq=1; return(checksoftirq(0));
#endif
      OPTARG("--increasetime","-it") increasetime=OPTARG_GETINT();
      OPTARG("--checktime","-ct") checktime=OPTARG_GETINT();
      OPTARG("--waittime","-wt") waittime=OPTARG_GETINT();
      OPTARG("--test","-te") testing=1; verbose=1;
      OPTARG("--version","-ve") printf("Das Version die Uhr Hund %s nach sein bist.\n",VERSION);exit(0);
    }OPTARGS_END;
  

  // Logging to /var/log/messages
  {
    openlog("das_watchdog", 0, LOG_DAEMON);
    syslog(LOG_INFO, "started");
  }
  
  // Check various.
  {
#if TIMERCHECKS    
    if(force && checksoftirq(force)<0)
      return -2;

    checksoftirq(force);
#endif

    if(getuid()!=0){
      print_error(stdout,"Warning, you are not running as root. das_watchdog should be run as an init-script at startup, and not as a normal user.\n");
    }
    
    
    if(access(WHICH_XMESSAGE,X_OK)!=0){
      print_error(stderr,"Warning, \"xmessage\" is not found or is not an executable. I will try to use the $PATH instead. Hopefully that'll work,");
      print_error(stderr,"but you might not receive messages to the screen in case das_watchdog has to take action.");
      xmessage_found=0;
    }
  }


  // Set priority
  if(1)  {
    if( ! set_pid_priority(0,SCHED_FIFO,sched_get_priority_max(SCHED_FIFO),
			   "Unable to set SCHED_FIFO realtime priority for %d (\"%s\"). (%s). Exiting.",
			   "Der Gewinde nach die Uhr Hund"))
      return 0;
    if(mlockall(MCL_CURRENT|MCL_FUTURE)==-1)
      print_error(stderr,"Could not call mlockalll(MCL_CURRENT|MCL_FUTURE) (%s)",strerror(errno));
  }


  // Start child thread.
  {
    pthread_create(&counter_thread,NULL,counter_func,NULL);
  }


  // Main loop. (We are never supposed to exit from this one.)
  for(;;){
    int lastcounter=counter;

    sleep(checktime);
    if(verbose)
      print_error(stderr,"    counter read to be %d  (lastcounter=%d)",counter,lastcounter);
    
    if(lastcounter==counter || testing==1){
      int changedsched=0;
      struct proclistlist *pll=pll_create();
      int lokke;

      if(verbose)
	print_error(stdout,"Die Uhr Hund stossen sein!");

      for(lokke=0;lokke<pll->length;lokke++){
	if(pll->proclist[lokke].policy!=SCHED_OTHER 
	   && pll->proclist[lokke].pid!=mypid 
	   && (!is_a_member(pll->proclist[lokke].pid,timerpids,num_cpus))
	   )
	  {
	    struct sched_param par={0};
	    par.sched_priority=0;
	    if(verbose)
	      print_error(stdout,"Setting pid %d temporarily to SCHED_OTHER.",pll->proclist[lokke].pid);
	    if(set_pid_priority(pll->proclist[lokke].pid,SCHED_OTHER,0,"Could not set pid %d (\"%s\") to SCHED_OTHER (%s).\n","no name"))
	      changedsched++;
	  }
      }

      if(changedsched>0 || testing==1){

        print_error(NULL,"realtime operations paused for %d seconds.",waittime);

	if(fork()==0){
	  xmessage_fork(pll);
	  return 0;
	}

	sleep(waittime);

	for(lokke=0;lokke<pll->length;lokke++){
	  if(pll->proclist[lokke].policy != SCHED_OTHER 
	     && pll->proclist[lokke].pid != mypid 
	     && (!is_a_member(pll->proclist[lokke].pid,timerpids,num_cpus))
	     && pll->proclist[lokke].start_time == get_pid_start_time(pll->proclist[lokke].pid) 
	     )
	    {
	      if(get_pid_priority(pll->proclist[lokke].pid)      != 0
                 || sched_getscheduler(pll->proclist[lokke].pid) != SCHED_OTHER){
		print_error(stderr,
			    "Seems like someone else has changed priority and/or scheduling policy for %d in the mean time. I'm not going to do anything.",
			    pll->proclist[lokke].pid);
	      }else{
		struct sched_param par={0};
		par.sched_priority=pll->proclist[lokke].priority;
		if(verbose)
		  print_error(stdout,"Setting pid %d back to realtime priority.",pll->proclist[lokke].pid);
		set_pid_priority(pll->proclist[lokke].pid,pll->proclist[lokke].policy,pll->proclist[lokke].priority,"Could not set pid %d (\"%s\") to SCHED_FIFO/SCHED_RR (%s).\n\n", "no name");
	      }
	    }
	}
      }
      pll_delete(pll);
    }
    if(testing==1) break;
  }
  
  return 0;
}


