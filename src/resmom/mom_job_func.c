/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/
/*
 * Functions which provide basic operation on the job structure
 *
 * Included public functions are:
 *
 *   job_alloc    allocate job struct and initialize defaults
 *   mom_job_free   free space allocated to the job structure and its
 *    childern structures.
 *   mom_job_purge   purge job from server
 *
 *   job_unlink_file() unlinks a given file using job credentials
 *
 * Include private function:
 *   job_init_wattr() initialize job working attribute array to "unspecified"
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/param.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#ifndef SIGKILL
#include <signal.h>
#endif
#if __STDC__ != 1
#include <memory.h>
#endif

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <set>
#include <semaphore.h>
#include <arpa/inet.h>

#include "pbs_ifl.h"
#include "list_link.h"
#include "work_task.h"
#include "attribute.h"
#include "resource.h"
#include "server_limits.h"
#include "server.h"
#include "queue.h"
#include "pbs_job.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "pbs_error.h"
#include "svrfunc.h"
#include "acct.h"
#include "net_connect.h"
#include "portability.h"
#include "threadpool.h"
#include "alps_functions.h"
#include "alps_constants.h"
#include "dis.h"
#include "mutex_mgr.hpp"
#ifdef PENABLE_LINUX26_CPUSETS
#include "pbs_cpuset.h"
#endif
#include "utils.h"
#include "mom_config.h"
#include "container.hpp"
#include "mom_job_cleanup.h"
#include "mom_func.h"
#include "node_frequency.hpp"

#ifdef PENABLE_LINUX_CGROUPS
#include "trq_cgroups.h"
#include "machine.hpp"
#endif

#ifdef ENABLE_PMIX
extern "C"{
#include "pmix_server.h"
}
#include "utils.h"

extern std::string topology_xml;
extern char        mom_alias[];
#endif

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

extern pthread_mutex_t  delete_job_files_mutex;

int conn_qsub(char *, long, char *);

/* External functions */
extern void mom_checkpoint_delete_files(job_file_delete_info *);

#if IBM_SP2==2  /* IBM SP PSSP 3.1 */
void unload_sp_switch(job *pjob);
#endif   /* IBM SP */

#ifdef PENABLE_LINUX26_CPUSETS
extern int use_cpusets(job *);
#endif /* PENABLE_LINUX26_CPUSETS */
/* Local Private Functions */

static void job_init_wattr(job *);

/* Global Data items */

extern gid_t   pbsgroup;
extern uid_t   pbsuser;
extern char   *msg_abt_err;
extern char   *path_jobs;
extern char   *path_spool;
extern char   *path_aux;
extern char   *msg_err_purgejob;
extern char    server_name[];
extern time_t  time_now;

extern tlist_head svr_newjobs;

extern job_pid_set_t global_job_sid_set;

extern sem_t *delete_job_files_sem;

void nodes_free(job *);

extern void MOMCheckRestart(void);
void       send_update_soon();


extern int multi_mom;
extern int pbs_rm_port;


task::~task()
  {
  if (this->ti_chan != NULL)
    {
    close_conn(this->ti_chan->sock, FALSE);
    DIS_tcp_cleanup(this->ti_chan);
    }
  } // END task destructor





/*
 * remtree - remove a tree (or single file)
 *
 * returns  0 on success
 *  -1 on failure
 */

int remtree(

  char *dirname)

  {
  DIR           *dir;

  struct dirent *pdir;
  char           namebuf[MAXPATHLEN];
  int            len;
  int            rtnv = 0;
#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  struct stat64  sb;
#else

  struct stat    sb;
#endif

#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)

  if (lstat64(dirname, &sb) == -1)
#else
  if (lstat(dirname, &sb) == -1)
#endif
    {

    if (errno != ENOENT)
      log_err(errno, __func__, (char *)"stat");

    return(-1);
    }

  if (S_ISDIR(sb.st_mode))
    {
    if ((dir = opendir(dirname)) == NULL)
      {
      if (errno != ENOENT)
        log_err(errno, __func__, (char *)"opendir");

      return(-1);
      }

    snprintf(namebuf, sizeof(namebuf), "%s/", dirname);

    len = strlen(namebuf);

    while ((pdir = readdir(dir)) != NULL)
      {
      if (pdir->d_name[0] == '.' && (pdir->d_name[1] == '\0' ||
         (pdir->d_name[1] == '.' && pdir->d_name[2] == '\0')))
        continue;

      snprintf(namebuf + len, sizeof(namebuf) - len, "%s", pdir->d_name);

#if defined(HAVE_STRUCT_STAT64) && defined(HAVE_STAT64) && defined(LARGEFILE_WORKS)
      if (lstat64(namebuf, &sb) == -1)
#else
      if (lstat(namebuf, &sb) == -1)
#endif
        {
        log_err(errno, __func__, (char *)"stat");

        rtnv = -1;

        continue;
        }

      if (S_ISDIR(sb.st_mode))
        {
        rtnv = remtree(namebuf);
        }
      else if (unlink_ext(namebuf) < 0)
        {
        sprintf(log_buffer, "unlink failed on %s", namebuf);
        log_err(errno, __func__, log_buffer);
        
        rtnv = -1;
        }
      else if (LOGLEVEL >= 7)
        {
        sprintf(log_buffer, "unlink(1) succeeded on %s", namebuf);

        log_ext(-1, __func__, log_buffer, LOG_DEBUG);
        }
      }    /* END while ((pdir = readdir(dir)) != NULL) */

    closedir(dir);

    if (rmdir_ext(dirname, 10) < 0)
      {
      if ((errno != ENOENT) && (errno != EINVAL))
        {
        sprintf(log_buffer, "rmdir failed on %s",
                dirname);

        log_err(errno, __func__, log_buffer);

        rtnv = -1;
        }
      }
    else if (LOGLEVEL >= 7)
      {
      sprintf(log_buffer, "rmdir succeeded on %s", dirname);

      log_ext(-1, __func__, log_buffer, LOG_DEBUG);
      }
    }
  else if (unlink_ext(dirname) < 0)
    {
    snprintf(log_buffer,sizeof(log_buffer),"unlink failed on %s",dirname);
    log_err(errno,__func__,log_buffer);

    rtnv = -1;
    }
  else if (LOGLEVEL >= 7)
    {
    sprintf(log_buffer, "unlink(2) succeeded on %s", dirname);

    log_ext(-1, __func__, log_buffer, LOG_DEBUG);
    }

  return(rtnv);
  }  /* END remtree() */



/*
 * conn_qsub - connect to the qsub that submitted this interactive job
 * return >= 0 on SUCCESS, < 0 on FAILURE
 * (this was moved from resmom/mom_inter.c)
 */



int conn_qsub(

  char *hostname,  /* I */
  long  port,      /* I */
  char *EMsg)      /* O (optional,minsize=1024) */

  {
  pbs_net_t hostaddr;
  
  int       s;
  int       local_errno = 0;
  int flags;


  if (EMsg != NULL)
    EMsg[0] = '\0';

  if ((hostaddr = get_hostaddr(&local_errno, hostname)) == (pbs_net_t)0)
    {
#if !defined(H_ERRNO_DECLARED) && !defined(_AIX)
    extern int h_errno;
#endif

    /* FAILURE */

    if (EMsg != NULL)
      {
      snprintf(EMsg, 1024, "cannot get address for host '%s', h_errno=%d",
               hostname,
               h_errno);
      }

    return(-1);
    }

  s = client_to_svr(hostaddr, (unsigned int)port, 0, EMsg);

  /* NOTE:  client_to_svr() can return 0 for SUCCESS */

  /* assume SUCCESS requires s > 0 (USC) was 'if (s >= 0)' */
  /* above comment not enabled */

  if (s < 0)
    {
    /* FAILURE */
    char remote_addr[128];
    socklen_t  size = 128;
    uint32_t net_order = ntohl(hostaddr);

    inet_ntop(AF_INET, (const void *)&net_order, remote_addr, size);

    snprintf(EMsg, 1024, "Failed to connect to %s at address %s:%d",
             hostname,
             remote_addr,
             (int)port);

    return(-1);
    }

  /* SUCCESS */

  /* this socket should be blocking */

  flags = fcntl(s, F_GETFL);

  flags &= ~O_NONBLOCK;

  fcntl(s, F_SETFL, flags);

  return(s);
  }  /* END conn_qsub() */




/*
 * job_alloc - allocate space for a job structure and initialize working
 * attribute to "unset"
 *
 * Returns: pointer to structure or null is space not available.
 */

job *job_alloc(void)

  {
  job *pj;

  pj = (job *)calloc(1, sizeof(job));

  if (pj == NULL)
    {
    log_err(errno, "job_alloc", (char *)"no memory");

    return(NULL);
    }

  pj->ji_qs.qs_version = PBS_QS_VERSION;

  CLEAR_LINK(pj->ji_jobque);

  pj->ji_tasks = new std::vector<task *>();
  pj->ji_usages = new std::map<std::string, job_host_data>();
  pj->ji_taskid = TM_NULL_TASK + 1;
  pj->ji_obit = TM_NULL_EVENT;
  pj->ji_nodekill = TM_ERROR_NODE;
  pj->ji_stats_done = false;

  pj->ji_momhandle = -1;  /* mark mom connection invalid */
#ifdef PENABLE_LINUX_CGROUPS
  pj->ji_cgroups_created = false;
#endif

  pj->ji_sigtermed_processes = new std::set<int>();

  /* set the working attributes to "unspecified" */
  job_init_wattr(pj);

  pj->ji_job_pid_set = new job_pid_set_t;

  return(pj);
  }  /* END job_alloc() */





/*
 * mom_job_free - free job structure and its various sub-structures
 */

void mom_job_free(

  job *pj)  /* I (modified) */

  {
  int    i;

  if (LOGLEVEL >= 8)
    {
    sprintf(log_buffer, "freeing job");

    log_record(PBSEVENT_DEBUG,
               PBS_EVENTCLASS_JOB,
               pj->ji_qs.ji_jobid,
               log_buffer);
    }

  /* remove any calloc working attribute space */

  for (i = 0;i < JOB_ATR_LAST;i++)
    {
    job_attr_def[i].at_free(&pj->ji_wattr[i]);
    }

  if (pj->ji_grpcache)
    free(pj->ji_grpcache);

  assert(pj->ji_preq == NULL);

  nodes_free(pj);

  // Delete each remaining task
  for (unsigned int i = 0; i < pj->ji_tasks->size(); i++)
    delete pj->ji_tasks->at(i);

  delete pj->ji_tasks;

  if (pj->ji_resources)
    {
    free(pj->ji_resources);
    pj->ji_resources = NULL;
    }

  delete pj->ji_job_pid_set;
  delete pj->ji_sigtermed_processes;

  /* now free the main structure */
  free(pj);

  return;
  }  /* END mom_job_free() */


 /*
 * job_unlink_file - unlink file, but drop root credentials before
 * doing this to avoid removing objects that aren't belong to the user.
 */
int job_unlink_file(

  job        *pjob,  /* I */
  const char *name)	 /* I */

  {
  int   saved_errno = 0;
  int   result = 0;
  uid_t uid = geteuid();
  gid_t gid = getegid();

  if (uid != 0)
    return unlink_ext(name);

  if ((setegid(pjob->ji_qs.ji_un.ji_momt.ji_exgid) == -1))
    return -1;
  if ((setuid_ext(pjob->ji_qs.ji_un.ji_momt.ji_exuid, TRUE) == -1))
    {
    saved_errno = errno;
    setegid(gid);
    errno = saved_errno;
    return -1;
    }
  result = unlink_ext(name);
  saved_errno = errno;

  setuid_ext(uid, TRUE);
  setegid(gid);

  errno = saved_errno;
  return(result);
  }  /* END job_unlink_file() */




/*
 * job_init_wattr - initialize job working attribute array
 * set the types and the "unspecified value" flag
 */

static void job_init_wattr(

  job *pj)

  {
  int i;

  for (i = 0; i < JOB_ATR_LAST; i++)
    {
    clear_attr(&pj->ji_wattr[i], &job_attr_def[i]);
    }

  return;
  }   /* END job_init_wattr() */


void remove_tmpdir_file(

    job_file_delete_info *jfdi)

  {
  char                  namebuf[MAXPATHLEN];
  int                   rc = 0;

  if (jfdi->has_temp_dir == TRUE)
    {
    if (tmpdir_basename[0] == '/')
      {
      snprintf(namebuf, sizeof(namebuf), "%s/%s", tmpdir_basename, jfdi->jobid);
      sprintf(log_buffer, "removing transient job directory %s",
        namebuf);

      log_record(PBSEVENT_DEBUG,PBS_EVENTCLASS_JOB,jfdi->jobid,log_buffer);

      if ((setegid(jfdi->gid) == -1) ||
          (setuid_ext(jfdi->uid, TRUE) == -1))
        {
        /* FAILURE */
        rc = -1;;
        }
      else
        {
        rc = remtree(namebuf);
        if (rc != 0)
          {
          sprintf(log_buffer, "remtree failed: %s", strerror(errno));
          log_err(errno, __func__, log_buffer);
          }
        
        setuid_ext(pbsuser, TRUE);
        setegid(pbsgroup);
        }
      
      if ((rc != 0) && 
          (LOGLEVEL >= 5))
        {
        sprintf(log_buffer,
          "recursive remove of job transient tmpdir %s failed",
          namebuf);
        
        log_err(errno, "recursive (r)rmdir", log_buffer);
        }
      }
    } /* END code to remove temp dir */
  }




void *delete_job_files(

  void *vp)

  {
  job_file_delete_info *jfdi = (job_file_delete_info *)vp;
  char                  namebuf[MAXPATHLEN];
  int                   rc = 0;
  char                  log_buf[LOCAL_LOG_BUF_SIZE];

  if (thread_unlink_calls == true)
    {
    /* this algorithm needs to make sure the 
       thread for delete_job_files posts to the 
       semaphore before it tries to lock the
       delete_job_files_mutex. posting to delete_job_files_sem
       lets other processes know there are threads still
       cleaning up jobs
     */
    rc = sem_post(delete_job_files_sem);
    if (rc)
      {
      log_err(-1, __func__, "failed to post delete_job_files_sem");
      }
    
    pthread_mutex_lock(&delete_job_files_mutex);
    }
#ifdef PENABLE_LINUX26_CPUSETS
  /* Delete the cpuset for the job. */
  delete_cpuset(jfdi->jobid, true);
#endif /* PENABLE_LINUX26_CPUSETS */

#ifdef PENABLE_LINUX_CGROUPS
  /* We need to remove the cgroup hierarchy for this job */
  trq_cg_delete_job_cgroups(jfdi->jobid, jfdi->cgroups_all_created);

  if (LOGLEVEL >=6)
    {
    sprintf(log_buf, "removed cgroup of job %s.", jfdi->jobid);
    log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
    }
#endif

  /* delete the node file and gpu file */
  sprintf(namebuf,"%s/%s", path_aux, jfdi->jobid);
  unlink_ext(namebuf);
  
  sprintf(namebuf, "%s/%sgpu", path_aux, jfdi->jobid);
  unlink_ext(namebuf);
  
  sprintf(namebuf, "%s/%smic", path_aux, jfdi->jobid);
  unlink_ext(namebuf);

  /* delete script file */
  if (multi_mom)
    {
    snprintf(namebuf, sizeof(namebuf), "%s%s%d%s",
      path_jobs,
      jfdi->prefix,
      pbs_rm_port,
      JOB_SCRIPT_SUFFIX);
    }
  else
    {
    snprintf(namebuf, sizeof(namebuf), "%s%s%s",
      path_jobs,
      jfdi->prefix,
      JOB_SCRIPT_SUFFIX);
    }

  if (unlink_ext(namebuf) < 0)
    {
    snprintf(log_buf, sizeof(log_buf), "Failed to remove '%s' for job '%s'",
      namebuf, jfdi->jobid);
    log_err(errno, __func__, log_buf);
    }
  else
    {
    snprintf(log_buf, sizeof(log_buf), "removed job script");
    log_record(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, jfdi->jobid, log_buf);
    }

  /* delete job task directory */
  if (multi_mom)
    {
    snprintf(namebuf,sizeof(namebuf),"%s%s%d%s",
      path_jobs,
      jfdi->prefix,
      pbs_rm_port,
      JOB_TASKDIR_SUFFIX);
    }
  else
    {
    snprintf(namebuf,sizeof(namebuf),"%s%s%s",
      path_jobs,
      jfdi->prefix,
      JOB_TASKDIR_SUFFIX);
    }

  remtree(namebuf);

  mom_checkpoint_delete_files(jfdi);

  /* delete job file */
  if (multi_mom)
    {
    snprintf(namebuf,sizeof(namebuf),"%s%s%d%s",
      path_jobs,
      jfdi->prefix,
      pbs_rm_port,
      JOB_FILE_SUFFIX);
    }
  else
    {
    snprintf(namebuf,sizeof(namebuf),"%s%s%s",
      path_jobs,
      jfdi->prefix,
      JOB_FILE_SUFFIX);
    }

  if (unlink_ext(namebuf) < 0)
    {
    snprintf(log_buf, sizeof(log_buf), "Failed to remove '%s' for job '%s'",
      namebuf, jfdi->jobid);
    log_err(errno, __func__, log_buf);
    }
  else if (LOGLEVEL >= 6)
    {
    snprintf(log_buf, sizeof(log_buf), "removed job file");
    log_record(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, jfdi->jobid, log_buf);
    }

  free(jfdi);

  if (thread_unlink_calls == true)
    {
    /* decrement the delte_job_files_sem so 
       other threads know this job is done
       cleaning up 
     */
    sem_wait(delete_job_files_sem);
    pthread_mutex_unlock(&delete_job_files_mutex);
    }
  return(NULL);
  } /* END delete_job_files() */





int release_job_reservation(

  job *pjob)

  {
  int   rc = PBSE_NONE;
  char *rsv_id;

  /* release this job's reservation */
  if ((pjob->ji_wattr[JOB_ATR_reservation_id].at_flags & ATR_VFLAG_SET) &&
      (pjob->ji_wattr[JOB_ATR_reservation_id].at_val.at_str != NULL))
    {
    rsv_id = pjob->ji_wattr[JOB_ATR_reservation_id].at_val.at_str;

    if ((rc = destroy_alps_reservation(rsv_id, apbasil_path, apbasil_protocol, APBASIL_RETRIES)) != PBSE_NONE)
      {
      snprintf(log_buffer, sizeof(log_buffer), "Couldn't release reservation for job %s",
        pjob->ji_qs.ji_jobid);
      log_err(-1, __func__, log_buffer);
      }
    }

  return(rc);
  } /* END release_job_reservation() */



void remove_from_exiting_list(

  job *pjob)

  {
  /* remove the job from the exiting_job_list */
  for (unsigned int i = 0; i < exiting_job_list.size(); i++)
    {
    if (exiting_job_list[i].jobid == pjob->ji_qs.ji_jobid)
      {
      exiting_job_list.erase(exiting_job_list.begin() + i);
      break;
      }
    } 
  } /* END remove_from_exiting_list() */



void remove_from_job_list(

  job *pjob)

  {
  alljobs_list.remove(pjob);
  } // END remove_from_job_list()



void mom_job_purge(

  job *pjob)  /* I (modified) */

  {
  job_file_delete_info *jfdi;

  jfdi = (job_file_delete_info *)calloc(1, sizeof(job_file_delete_info));

  if (jfdi == NULL)
    {
    log_err(ENOMEM,__func__, (char *)"No space to allocate info for job file deletion");
    return;
    }

#ifdef NVIDIA_GPUS
  /*
   * Did this job have a gpuid assigned?
   * if so, then update gpu status
   */
  if (((pjob->ji_wattr[JOB_ATR_exec_gpus].at_flags & ATR_VFLAG_SET) != 0) &&
      (pjob->ji_wattr[JOB_ATR_exec_gpus].at_val.at_str != NULL))
    {
    send_update_soon();
    }
#endif  /* NVIDIA_GPUS */

  /* initialize struct information */
  if (pjob->ji_flags & MOM_HAS_TMPDIR)
    {
    jfdi->has_temp_dir = TRUE;
    pjob->ji_flags &= ~MOM_HAS_TMPDIR;
    }
  else
    jfdi->has_temp_dir = FALSE;

  strcpy(jfdi->jobid,pjob->ji_qs.ji_jobid);
  strcpy(jfdi->prefix,pjob->ji_qs.ji_fileprefix);

  if ((pjob->ji_wattr[JOB_ATR_checkpoint_dir].at_flags & ATR_VFLAG_SET) &&
      (pjob->ji_wattr[JOB_ATR_checkpoint_name].at_flags & ATR_VFLAG_SET))
    jfdi->checkpoint_dir = strdup(pjob->ji_wattr[JOB_ATR_checkpoint_dir].at_val.at_str);

  jfdi->gid = pjob->ji_qs.ji_un.ji_momt.ji_exgid;
  jfdi->uid = pjob->ji_qs.ji_un.ji_momt.ji_exuid;

#ifdef PENABLE_LINUX_CGROUPS
  jfdi->cgroups_all_created = pjob->ji_cgroups_created;
#endif

  /* remove each pid in ji_job_pid_set from the global_job_sid_set */
  for (job_pid_set_t::const_iterator job_pid_set_iter = pjob->ji_job_pid_set->begin();
       job_pid_set_iter != pjob->ji_job_pid_set->end();
       job_pid_set_iter++)
    {
    /* get pid entry from ji_job_pid_set */
    pid_t job_pid = *job_pid_set_iter;

    /* see if job_pid exists in job_sid set */
    job_pid_set_t::const_iterator it = global_job_sid_set.find(job_pid);
    if (it != global_job_sid_set.end())
      {
      /* remove job_pid from the set */
      global_job_sid_set.erase(it);
      }
    }

  remove_tmpdir_file(jfdi);

  if (thread_unlink_calls == true)
    enqueue_threadpool_request(delete_job_files, jfdi, request_pool);
  else
    delete_job_files(jfdi);

  /* remove this job from the global queue */
  delete_link(&pjob->ji_jobque);

  remove_from_job_list(pjob);

  remove_from_exiting_list(pjob);

  if (LOGLEVEL >= 6)
    {
    sprintf(log_buffer,"removing job");

    log_record(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, pjob->ji_qs.ji_jobid, log_buffer);
    }

#if IBM_SP2==2        /* IBM SP PSSP 3.1 */
  unload_sp_switch(pjob);

#endif   /* IBM SP */

  //We had a request to change the frequency for the job and now that the job is done
  //we want to change the frequency back.
  resource *presc = find_resc_entry(&pjob->ji_wattr[JOB_ATR_resource],
            find_resc_def(svr_resc_def, "cpuclock", svr_resc_size));
  if (presc != NULL)
    {
    std::string beforeFreq;

    nd_frequency.get_frequency_string(beforeFreq);
    if(!nd_frequency.restore_frequency())
      {
      std::string msg = "Failed to restore frequency.";
      log_ext(nd_frequency.get_last_error(),__func__,msg.c_str(),LOG_ERR);
      }
    else
      {
      std::string afterFreq;
      nd_frequency.get_frequency_string(afterFreq);
      std::string msg = "Restored frequency from " + beforeFreq + " to " + afterFreq;
      log_ext(PBSE_CHANGED_CPU_FREQUENCY,__func__, msg.c_str(),LOG_NOTICE);
      }
    }

  /*delete pjob->ji_job_pid_set;*/
  mom_job_free(pjob);

  /* if no jobs are left, check if MOM should be restarted */

  if (alljobs_list.size() == 0)
    MOMCheckRestart();

  return;
  }  /* END mom_job_purge() */





/*
 * mom_find_job() - find job by jobid
 *
 * Search list of all server jobs for one with same jobid
 * Return NULL if not found or pointer to job struct if found
 */

job *mom_find_job(

  const char *jobid)

  {
  std::string  jid(jobid);
  job         *pj;
  std::size_t  pos = 0;

  if ((pos = jid.find("@")) != std::string::npos)
    jid.erase(pos);

  std::list<job *>::iterator iter;

  for (iter = alljobs_list.begin(); iter != alljobs_list.end(); iter++)
    {
    pj = *iter;

    // Match
    if (jid == pj->ji_qs.ji_jobid)
      return(pj);
    }

  return(NULL);
  }   /* END mom_find_job() */



/*
 * mom_find_job_by_int_id()
 *
 * Finds a job using just the integer portion of the job id
 * @return the job, or NULL if no local job has that integer id
 */

job *mom_find_job_by_int_string(

  const char *jobint_string)

  {
  job  *pjob;
  int   job_id_buf_len;

  job_id_buf_len = strlen(jobint_string);

  std::list<job *>::iterator iter;

  for (iter = alljobs_list.begin(); iter != alljobs_list.end(); iter++)
    {
    pjob = *iter;

    // Compare just the numeric portion of the id
    if (!strncmp(pjob->ji_qs.ji_jobid, jobint_string, job_id_buf_len))
      {
      // Make sure the job's numeric portion as ended
      if ((pjob->ji_qs.ji_jobid[job_id_buf_len] == '.') ||
          (pjob->ji_qs.ji_jobid[job_id_buf_len] == '\0'))
      // Found the job
      return(pjob);
      }
    }

  // Not found - we return from the loop if we find the job
  return(NULL);
  } // END mom_find_job_by_int_string()



/*
 * am_i_mother_superior()
 *
 * @return true if I am this job's mother superior, else false
 */
bool am_i_mother_superior(

  const job &pjob)

  {
  bool mother_superior = ((pjob.ji_nodeid == 0) && ((pjob.ji_qs.ji_svrflags & JOB_SVFLG_HERE) != 0));
    
  return(mother_superior);
  }


#ifdef ENABLE_PMIX
const int pmix_info_count = 17;

void free_info_array(

  pmix_status_t  pst,
  void          *cbdata)

  {
  pmix_info_t *pmi_array = (pmix_info_t *)cbdata;

  PMIX_INFO_FREE(pmi_array, pmix_info_count);
  } // END free_info_array()



uint32_t local_arch = 0xFFFFFFFF;

void register_jobs_nspace(

  job *pjob)

  {
  pmix_info_t      *pmi_array;
  int               es = 0;
  int               lowest_rank = 0;
  int               prev_index = -1;
  int               attr_index;
  std::vector<int>  ranks;
  std::string       node_list;
 
  // Get some needed info about ranks / local execution slots 
  for (int i = 0; i < pjob->ji_numvnod; i++)
    {
    if (prev_index != pjob->ji_vnods[i].vn_host->hn_node)
      {
      if (node_list.size() != 0)
        node_list += ",";

      node_list += pjob->ji_vnods[i].vn_host->hn_host;
      }

    if (pjob->ji_vnods[i].vn_host->hn_node == pjob->ji_nodeid)
      {
      es++;

      ranks.push_back(i);
      }
    }

  if (ranks.size() != 0)
    lowest_rank = ranks[0];

  PMIX_INFO_CREATE(pmi_array, pmix_info_count);

  attr_index = 0;
  strcpy(pmi_array[attr_index].key, PMIX_JOBID);
  pmi_array[attr_index].value.type = PMIX_STRING;
  std::string jobid(pjob->ji_qs.ji_jobid);
  std::size_t dot = jobid.find(".");

  if (dot != std::string::npos)
    jobid.erase(dot);

  // PMIx just wants the numeric portion of the job id
  pmi_array[attr_index].value.data.string = strdup(jobid.c_str());

  // Add the application information for this job
  // Get the number of procs for this job
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_UNIV_SIZE);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = pjob->ji_numvnod;

  // For now, the job and namespace are the same size
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_JOB_SIZE);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = pjob->ji_numvnod;
  
  // Max procs for the job is also the same
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_MAX_PROCS);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = pjob->ji_numvnod;

  // Get the number of procs for this node
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_NODE_SIZE);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = es;
  
  // Local size is the same as the number for this node
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_LOCAL_SIZE);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = es;

  // Mapping information
  // list of nodes
  char *node_regex;
  PMIx_generate_regex(node_list.c_str(), &node_regex);
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_NODE_MAP);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = node_regex;

  // map of process ranks to nodes
  std::string ppn_list(pjob->ji_wattr[JOB_ATR_exec_host].at_val.at_str);
  std::replace(ppn_list.begin(), ppn_list.end(), '+', ';');
  std::replace(ppn_list.begin(), ppn_list.end(), '/', ',');
  char *ppn_regex;
  PMIx_generate_ppn(ppn_list.c_str(), &ppn_regex);
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_PROC_MAP);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = ppn_regex;

  // Node-level information
  // Node id
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_NODEID);
  pmi_array[attr_index].value.type = PMIX_UINT32;
  pmi_array[attr_index].value.data.uint32 = pjob->ji_nodeid;

  // hostname
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_HOSTNAME);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = strdup(mom_alias);

  // local peers
  std::string peer_ranks;
  translate_vector_to_range_string(peer_ranks, ranks);
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_LOCAL_PEERS);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = strdup(peer_ranks.c_str());

#ifdef ENABLE_CGROUPS
  // local cpuset
  if (pjob->ji_wattr[JOB_ATR_cpuset_string].at_val.at_str != NULL)
    {
    std::string range;
    std::string cpu_list(pjob->ji_wattr[JOB_ATR_cpuset_string].at_val.at_str);
    find_range_in_cpuset_string(cpu_list, range);
  
    attr_index++;
    strcpy(pmi_array[attr_index].key, PMIX_CPUSET);
    pmi_array[attr_index].value.type = PMIX_STRING;
    pmi_array[attr_index].value.data.uint32 = strdup(range.c_str());
    }

  // node topology
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_LOCAL_TOPO);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = strdup(topology_xml.c_str());
#endif

  // local leader
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_LOCALLDR);
  pmi_array[attr_index].value.type = PMIX_UINT64;
  pmi_array[attr_index].value.data.uint64 = lowest_rank;

  // architecture
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_ARCH);
  pmi_array[attr_index].value.type = PMIX_UINT64;
  pmi_array[attr_index].value.data.uint64 = local_arch;

  // top level directory for job
  char *init_dir = get_job_envvar(pjob, "PBS_O_INITDIR");
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_NSDIR);
  pmi_array[attr_index].value.type = PMIX_STRING;
  pmi_array[attr_index].value.data.string = strdup(init_dir);

  // temporary directory for job
  char tmpdir[MAXPATHLEN];
  attr_index++;
  strcpy(pmi_array[attr_index].key, PMIX_TMPDIR);
  pmi_array[attr_index].value.type = PMIX_STRING;
  if (TTmpDirName(pjob, tmpdir, sizeof(tmpdir)))
    pmi_array[attr_index].value.data.string = strdup(tmpdir);
  else
    pmi_array[attr_index].value.data.string = strdup(init_dir);

  PMIx_server_register_nspace(pjob->ji_qs.ji_jobid,
                              es,
                              pmi_array,
                              pmix_info_count,
                              free_info_array,
                              pmi_array);
  
  // Peer-level information - do we provide this at this time?
  // rank
  // appnum
  // application leader
  // global rank
  // application rank
  // local rank
  // node rank
  // node id
  // uri
  // cpuset for process
  // spawned - true if launched via a dynamic spawn
  // temporary dir for this process

  } // END register_jobs_nspace()

#endif

/* END job_func.c */

