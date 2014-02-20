#include <boost/ptr_container/ptr_vector.hpp>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>
#include "pbs_nodes.h"
#include "alps_constants.h"
#include <check.h>

int set_ncpus(struct pbsnode *,struct pbsnode *, int);
int set_ngpus(struct pbsnode *, int);
int set_state(struct pbsnode *, const char *);
void finish_gpu_status(boost::ptr_vector<std::string>::iterator& i,boost::ptr_vector<std::string>::iterator end);
struct pbsnode *create_alps_subnode(struct pbsnode *parent, const char *node_id);
struct pbsnode *find_alpsnode_by_name(struct pbsnode *parent, const char *node_id);
struct pbsnode *determine_node_from_str(const char *str, struct pbsnode *parent, struct pbsnode *current);
int check_if_orphaned(void *str);
int process_alps_status(char *, boost::ptr_vector<std::string>&);
int process_reservation_id(struct pbsnode *pnode, const char *rsv_id_str);
int record_reservation(struct pbsnode *pnode, const char *rsv_id);

char buf[4096];

char *alps_status = (char *)"node=1\0CPROC=12\0state=UP\0reservation_id=12\0<cray_gpu_status>\0gpu_id=0\0clock_mhz=2600\0gpu_id=1\0clock_mhz=2600\0</cray_gpu_status>\0\0";
/*node=2\0CPROC=12\0state=UP\0<cray_gpu_status>\0gpu_id=0\0clock_mhz=2600\0gpu_id=1\0clock_mhz=2600\0</cray_gpu_status>\0node=3\0CPROC=12\0state=UP\0<cray_gpu_status>\0gpu_id=0\0clock_mhz=2600\0gpu_id=1\0clock_mhz=2600\0</cray_gpu_status>\0\0";*/

extern int count;

START_TEST(record_reservation_test)
  {
  struct pbsnode pnode;

  memset(&pnode, 0, sizeof(pnode));

  fail_unless(record_reservation(&pnode, "1") != PBSE_NONE);

  job_usage_info *jui = (job_usage_info *)calloc(1, sizeof(job_usage_info));
  strcpy(jui->jobid, "1.napali");
  pnode.nd_job_usages.push_back(jui);
  fail_unless(record_reservation(&pnode, "1") == PBSE_NONE);
  }
END_TEST


START_TEST(set_ncpus_test)
  {
  struct pbsnode  pnode;
  struct pbsnode  parent;

  memset(&parent,0,sizeof(pbsnode));
  fail_unless(set_ncpus(&pnode,&parent, 2) == 0, "Couldn't set ncpus to 2");
  snprintf(buf, sizeof(buf), "ncpus should be 2 but is %d", pnode.nd_slots.get_total_execution_slots());
  fail_unless(pnode.nd_slots.get_total_execution_slots() == 2, buf);

  fail_unless(set_ncpus(&pnode,&parent, 4) == 0, "Couldn't set ncpus to 4");
  snprintf(buf, sizeof(buf), "ncpus should be 4 but is %d", pnode.nd_slots.get_total_execution_slots());
  fail_unless(pnode.nd_slots.get_total_execution_slots() == 4, buf);

  fail_unless(set_ncpus(&pnode,&parent, 8) == 0, "Couldn't set ncpus to 8");
  snprintf(buf, sizeof(buf), "ncpus should be 8 but is %d", pnode.nd_slots.get_total_execution_slots());
  fail_unless(pnode.nd_slots.get_total_execution_slots() == 8, buf);
  }
END_TEST




START_TEST(set_ngpus_test)
  {
  struct pbsnode pnode;

  memset(&pnode, 0, sizeof(pnode));

  fail_unless(set_ngpus(&pnode, 2) == 0, "Couldn't set ngpus to 2");
  snprintf(buf, sizeof(buf), "ngpus should be 2 but id %d", pnode.nd_ngpus);
  fail_unless(pnode.nd_ngpus == 2, buf);

  pnode.nd_ngpus = 0;
  fail_unless(set_ngpus(&pnode, 4) == 0, "Couldn't set ngpus to 4");
  snprintf(buf, sizeof(buf), "ngpus should be 4 but id %d", pnode.nd_ngpus);
  fail_unless(pnode.nd_ngpus == 4, buf);

  pnode.nd_ngpus = 0;
  fail_unless(set_ngpus(&pnode, 8) == 0, "Couldn't set ngpus to 8");
  snprintf(buf, sizeof(buf), "ngpus should be 8 but id %d", pnode.nd_ngpus);
  fail_unless(pnode.nd_ngpus == 8, buf);
  }
END_TEST





START_TEST(set_state_test)
  {
  struct pbsnode  pnode;
  const char    *up_str   = "state=UP";
  const char    *down_str = "state=DOWN";

  memset(&pnode, 0, sizeof(pnode));

  set_state(&pnode, up_str);
  snprintf(buf, sizeof(buf), "Couldn't set state to up, state is %d", pnode.nd_state);
  fail_unless(pnode.nd_state == INUSE_FREE, buf);
  set_state(&pnode, down_str);
  fail_unless((pnode.nd_state & INUSE_DOWN) != 0, "Couldn't set the state to down");

  set_state(&pnode, up_str);
  fail_unless(pnode.nd_state == INUSE_FREE, "Couldn't set the state to up 2");
  set_state(&pnode, down_str);
  fail_unless((pnode.nd_state & INUSE_DOWN) != 0, "Couldn't set the state to down 2");
  }
END_TEST




START_TEST(finish_gpu_status_test)
  {
  boost::ptr_vector<std::string> status;
  boost::ptr_vector<std::string>::iterator end;

  status.push_back(new std::string("o"));
  status.push_back(new std::string("n"));
  status.push_back(new std::string("</cray_gpu_status>"));
  status.push_back(new std::string("tom"));

  end = status.begin();
  finish_gpu_status(end,status.end());
  snprintf(buf, sizeof(buf), "penultimate string isn't correct, should be '%s' but is '%s'",
    CRAY_GPU_STATUS_END, end->c_str());
  fail_unless(!strcmp(end->c_str(), CRAY_GPU_STATUS_END), buf);

  end++;
  snprintf(buf, sizeof(buf), "last string isn't correct, should be 'tom' but is '%s'",
    end->c_str());
  fail_unless(!strcmp(end->c_str(), "tom"), buf);

  }
END_TEST




START_TEST(find_alpsnode_test)
  {
  struct pbsnode  parent;
  const char     *node_id = (char *)"tom";
  struct pbsnode *alpsnode;

  parent.alps_subnodes.allnodes_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  pthread_mutex_init(parent.alps_subnodes.allnodes_mutex, NULL);

  alpsnode = find_alpsnode_by_name(&parent, node_id);
  fail_unless(alpsnode == NULL, "returned a non-NULL node?");

  }
END_TEST




START_TEST(determine_node_from_str_test)
  {
  struct pbsnode  parent;
  const char     *node_str1 = "node=tom";
  const char     *node_str2 = "node=george";
  struct pbsnode *new_node;

  memset(&parent, 0, sizeof(parent));
  parent.nd_name = strdup("george");
  parent.alps_subnodes.allnodes_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  pthread_mutex_init(parent.alps_subnodes.allnodes_mutex, NULL);

  count = 0; // set so that create_alps_subnode doesn't fail
  new_node = determine_node_from_str(node_str1, &parent, &parent);
  fail_unless(new_node != NULL, "new node is NULL?");
  fail_unless(new_node->nd_lastupdate != 0, "update time not set");

  count = 0; // set so that create_alps_subnode doesn't fail
  new_node = determine_node_from_str(node_str2, &parent, &parent);
  fail_unless(new_node == &parent, "advanced current when current should've remained the same");

  }
END_TEST




START_TEST(check_orphaned_test)
  {
  const char *rsv_id = "tom";

  fail_unless(check_if_orphaned((void *)rsv_id) == 0, "bad return code");
  }
END_TEST




START_TEST(create_alps_subnode_test)
  {
  struct pbsnode  parent;
  const char     *node_id = "tom";
  struct pbsnode *subnode;
  extern int      svr_clnodes;
  int             start_clnodes_value = svr_clnodes;;

  memset(&parent, 0, sizeof(struct pbsnode));

  subnode = create_alps_subnode(&parent, node_id);
  fail_unless(subnode != NULL, "subnode was returned NULL?");
  fail_unless(subnode->parent == &parent, "parent set incorrectly");
  fail_unless(subnode->nd_ntype == NTYPE_CLUSTER, "node type incorrect");

  /* scaffolding makes it fail the second time */
  subnode = create_alps_subnode(&parent, node_id);
  fail_unless(subnode == NULL, "subnode isn't NULL when it should be");
  fail_unless(start_clnodes_value + 2 <= svr_clnodes);
  }
END_TEST



START_TEST(whole_test)
  {
  boost::ptr_vector<std::string> ds;
  int             rc;
  
  ds.push_back(new std::string(alps_status));
 
  rc = process_alps_status((char *)"tom", ds);
  fail_unless(rc == 0, "didn't process alps status");
  }
END_TEST



START_TEST(process_reservation_id_test)
  {
  struct pbsnode pnode;

  memset(&pnode, 0, sizeof(struct pbsnode));

  fail_unless(process_reservation_id(&pnode, "12") == 0, "couldn't process reservation");
  fail_unless(process_reservation_id(&pnode, "13") == 0, "couldn't process reservation");
  fail_unless(process_reservation_id(&pnode, "14") == 0, "couldn't process reservation");
  }
END_TEST



Suite *node_func_suite(void)
  {
  Suite *s = suite_create("alps helper suite methods");
  TCase *tc_core = tcase_create("set_ncpus_test");
  tcase_add_test(tc_core, set_ncpus_test);
  suite_add_tcase(s, tc_core);
  
  tc_core = tcase_create("set_state_test");
  tcase_add_test(tc_core, set_state_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("finish_gpu_status_test");
  tcase_add_test(tc_core, finish_gpu_status_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("create_alps_subnode_test");
  tcase_add_test(tc_core, create_alps_subnode_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("find_alpsnode_test");
  tcase_add_test(tc_core, find_alpsnode_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("determine_node_from_str_test");
  tcase_add_test(tc_core, determine_node_from_str_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("check_orphaned_test");
  tcase_add_test(tc_core, check_orphaned_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("set_ngpus_test");
  tcase_add_test(tc_core, set_ngpus_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("whole_test");
  tcase_add_test(tc_core, whole_test);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("process_reservation_id_test");
  tcase_add_test(tc_core, process_reservation_id_test);
  tcase_add_test(tc_core, record_reservation_test);
  suite_add_tcase(s, tc_core);
  
  return(s);
  }

void rundebug()
  {
  }

int main(void)
  {
  int number_failed = 0;
  SRunner *sr = NULL;
  rundebug();
  sr = srunner_create(node_func_suite());
  srunner_set_log(sr, "node_func_suite.log");
  srunner_run_all(sr, CK_NORMAL);
  number_failed = srunner_ntests_failed(sr);
  srunner_free(sr);
  return(number_failed);
  }

