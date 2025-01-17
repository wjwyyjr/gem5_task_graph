/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Niket Agarwal
 *          Tushar Krishna
 */


#include "mem/ruby/network/garnet2.0/GarnetNetwork.hh"

#include <algorithm>
#include <cassert>

#include "base/cast.hh"
#include "base/stl_helpers.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/garnet2.0/CommonTypes.hh"
#include "mem/ruby/network/garnet2.0/CreditLink.hh"
#include "mem/ruby/network/garnet2.0/GarnetLink.hh"
#include "mem/ruby/network/garnet2.0/NetworkInterface.hh"
#include "mem/ruby/network/garnet2.0/NetworkLink.hh"
#include "mem/ruby/network/garnet2.0/Router.hh"
#include "mem/ruby/system/RubySystem.hh"

using namespace std;
using m5::stl_helpers::deletePointers;

/*
 * GarnetNetwork sets up the routers and links and collects stats.
 * Default parameters (GarnetNetwork.py) can be overwritten from command line
 * (see configs/network/Network.py)
 */

GarnetNetwork::GarnetNetwork(const Params *p)
    : Network(p), Consumer(this)
{
    m_num_rows = p->num_rows;
    m_ni_flit_size = p->ni_flit_size;
    m_vcs_per_vnet = p->vcs_per_vnet;
    m_vcs_for_allocation = p->vcs_for_allocation;
    m_buffers_per_data_vc = p->buffers_per_data_vc;
    m_buffers_per_ctrl_vc = p->buffers_per_ctrl_vc;
    m_routing_algorithm = p->routing_algorithm;
    m_task_graph_enable = p->task_graph_enable;
    m_task_graph_file = p->task_graph_file;
    m_token_packet_length = p->token_packet_length;
    m_topology = p->topology;
    m_architecture_file = p->architecture_file;
    m_print_task_execution_info = p->print_task_execution_info;
    m_vc_allocation_object = p->vc_allocation_object;
    m_in_mem_size = p->in_mem_size;
    m_out_mem_size = p->out_mem_size;

    m_enable_fault_model = p->enable_fault_model;
    if (m_enable_fault_model)
        fault_model = p->fault_model;

    m_vnet_type.resize(m_virtual_networks);

    for (int i = 0 ; i < m_virtual_networks ; i++) {
        if (m_vnet_type_names[i] == "response")
            m_vnet_type[i] = DATA_VNET_; // carries data (and ctrl) packets
        else
            m_vnet_type[i] = CTRL_VNET_; // carries only ctrl packets
    }

    // record the routers
    for (vector<BasicRouter*>::const_iterator i =  p->routers.begin();
         i != p->routers.end(); ++i) {
        Router* router = safe_cast<Router*>(*i);
        m_routers.push_back(router);

        // initialize the router's network pointers
        router->init_net_ptr(this);
    }

    // record the network interfaces
    for (vector<ClockedObject*>::const_iterator i = p->netifs.begin();
         i != p->netifs.end(); ++i) {
        NetworkInterface *ni = safe_cast<NetworkInterface *>(*i);
        m_nis.push_back(ni);
        ni->init_net_ptr(this);
    }

    //if topology is ring, vc must be a multiple of two
    if (m_topology == "Ring")
    {
        assert((m_vcs_per_vnet % 2 == 0) && (m_vcs_for_allocation % 2 == 0) &&\
        (m_vcs_for_allocation < m_vcs_per_vnet));
    }
}

void
GarnetNetwork::init()
{
    Network::init();

    for (int i=0; i < m_nodes; i++) {
        m_nis[i]->addNode(m_toNetQueues[i], m_fromNetQueues[i]);
    }

    // The topology pointer should have already been initialized in the
    // parent network constructor
    assert(m_topology_ptr != NULL);
    m_topology_ptr->createLinks(this);

    // Initialize topology specific parameters
    if (getNumRows() > 0) {
        // Only for Mesh topology
        // m_num_rows and m_num_cols are only used for
        // implementing XY or custom routing in RoutingUnit.cc
        m_num_rows = getNumRows();
        m_num_cols = m_routers.size() / m_num_rows;
        assert(m_num_rows * m_num_cols == m_routers.size());
    } else {
        m_num_rows = -1;
        m_num_cols = -1;
    }

    // FaultModel: declare each router to the fault model
    if (isFaultModelEnabled()) {
        for (vector<Router*>::const_iterator i= m_routers.begin();
             i != m_routers.end(); ++i) {
            Router* router = safe_cast<Router*>(*i);
            int router_id M5_VAR_USED =
                fault_model->declare_router(router->get_num_inports(),
                                            router->get_num_outports(),
                                            router->get_vc_per_vnet(),
                                            getBuffersPerDataVC(),
                                            getBuffersPerCtrlVC());
            assert(router_id == router->get_id());
            router->printAggregateFaultProbability(cout);
            router->printFaultVector(cout);
        }
    }

    // Task Graph Intinalization
    if (isTaskGraphEnabled()){
        task_start_time_vs_id = simout.\
            create("task_start_time_vs_id.log", false, true);
        task_start_end_time_vs_id = simout.\
            create("task_start_end_time_vs_id.log", false, true);
        task_start_time_vs_id_iters = simout.\
            create("task_start_time_vs_id_iters.log", false, true);
        throughput_info = simout.open("throughput.log", ios_base::out|ios_base::app, false, true);
        *(throughput_info->stream())<<"Simulation_Time\tExectution_Times (Application_Packet)\tThroughput(packet/s)\t(Note! Just for first Application !)"<<endl;
        //for app ete delay out-of-order when run simulation.
        //app_delay_running_info = simout.create("application_delay_running_info.log", false, true);
        app_delay_running_info = simout.open("application_delay_running_info.log",ios_base::out|ios_base::app, false, true);
        *(app_delay_running_info->stream())<<"Application\tIteration\tStart_time\tEnd_time\tExecution_Delay"<<endl;

        network_performance_info = simout.open("network_performance.log",ios_base::out|ios_base::app, false, true);
        *(network_performance_info->stream())<<"Application\tIteration\tAverage_Flit_Latency\tAverage_Flit_Network_Latency\tAverage_Flit_Queueing_Latency\tFlits_Received\tAverage_Flit_Hops"<<endl;

        task_waiting_time_info = simout.create("task_waiting_time_info.log", false, true);

        // start_time_info = simout.open("start_time_info.log",ios_base::out|ios_base::app, false, true);
        // end_time_info = simout.open("end_time_info.log",ios_base::out|ios_base::app, false, true);
        // ete_info = simout.open("ete_info.log",ios_base::out|ios_base::app, false, true);

        // write here because we need the total number of application before
        //configure nodes
        DPRINTF(TaskGraph, "Start Load Application Configuration !\n");
        if (readApplicationConfig(m_task_graph_file))
            cout<<"info: Load Application Configuration -"<<m_task_graph_file\
                <<" - successfully !"<<endl;
        //Construct Nodes
        DPRINTF(TaskGraph, "Start Construct Nodes !\n");
        if (constructArchitecture(m_architecture_file))
            cout<<"info: Construct Node -"<<m_architecture_file<<\
            " - successfully !"<<endl;

        //num_completed_tasks[num_apps][num_iters] records the num tasks in this iters
        current_execution_iterations = new int[m_num_application];
        num_completed_tasks = new int* [m_num_application];        
        for(int i=0;i<m_num_application;i++){
            num_completed_tasks[i] = new int[m_applicaton_execution_iterations[i]];
        }

        for(int i=0;i<m_num_application;i++){
            current_execution_iterations[i] = 0;
            for(int j=0;j<m_applicaton_execution_iterations[i];j++){
                num_completed_tasks[i][j] = 0;
            }
        }        

        //Print Node Configuration Information
        if (true){
            cout<<"\n";
            for (int i=0;i<m_nodes/2;i++)
                m_nis[i]->printNodeConfiguation();
            cout<<"\n";
        }

        //load traffic by the task graph file.
        head_task.resize(m_num_application);
        DPRINTF(TaskGraph, "Start Load Traffic !\n");
        if (loadTraffic(m_task_graph_file))
            cout<<"info: Load Traffic - "<<\
                m_task_graph_file<<" - successfully !"<<endl;

        for (int i=0;i<m_nodes/2;i++){
            m_nis[i]->initializeTaskIdList();
            // m_nis[i]->initializeTaskBuffer();
        }

        ETE_delay.resize(m_num_application);
        task_start_time.resize(m_num_application);
        task_end_time.resize(m_num_application);
        for(int i=0;i<m_num_application;i++){
            task_start_time[i].resize(m_applicaton_execution_iterations[i]);
            task_end_time[i].resize(m_applicaton_execution_iterations[i]);
            ETE_delay[i].resize(m_applicaton_execution_iterations[i]);
        }

        for(int i=0;i<m_num_application;i++)
            for(int j=0;j<m_applicaton_execution_iterations[i];j++){
                task_start_time[i][j] = INT_MAX;
                task_end_time[i][j] = INT_MIN;
            }

        //initialize the latency matrix
        src_dst_latency = new int* [m_num_core];
        for (int i=0;i<m_num_core;i++)
            src_dst_latency[i] = new int[m_num_core];

        for (int i=0;i<m_num_core;i++)
            for (int j=0;j<m_num_core;j++)
                src_dst_latency[i][j] = 0;
    }

    scheduleWakeupAbsolute(curCycle() + Cycles(1));
    //wake up the garnet network
}

GarnetNetwork::~GarnetNetwork()
{
    deletePointers(m_routers);
    deletePointers(m_nis);
    deletePointers(m_networklinks);
    deletePointers(m_creditlinks);

    delete [] m_application_name;
    delete [] m_applicaton_execution_iterations;
    delete [] m_num_task;
    delete [] m_num_edge;
    delete [] m_num_head_task;
    delete [] current_execution_iterations;

    for (int i=0;i<m_num_core;i++)
        delete [] src_dst_latency[i];
    delete [] src_dst_latency;

    for (int i=0;i<m_num_application;i++)
        delete [] num_completed_tasks[i];
    delete [] num_completed_tasks;
}

/*
 * This function creates a link from the Network Interface (NI)
 * into the Network.
 * It creates a Network Link from the NI to a Router and a Credit Link from
 * the Router to the NI
*/

//NodeID may the NI, SwitchID may the router
void
GarnetNetwork::makeExtInLink(NodeID src, SwitchID dest, BasicLink* link,
                            const NetDest& routing_table_entry)
{
    assert(src < m_nodes);

    GarnetExtLink* garnet_link = safe_cast<GarnetExtLink*>(link);

    // GarnetExtLink is bi-directional
    // each link includes two link, 0 is in , 1 is out
    NetworkLink* net_link = garnet_link->m_network_links[LinkDirection_In];
    net_link->setType(EXT_IN_);
    CreditLink* credit_link = garnet_link->m_credit_links[LinkDirection_In];

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    PortDirection dst_inport_dirn = "Local";
    m_routers[dest]->addInPort(dst_inport_dirn, net_link, credit_link);
    m_nis[src]->addOutPort(net_link, credit_link, dest);

    for (int i=0;i<m_nodes/2;i++){
        m_nis[i]->scheduleEventAbsolute(Cycles(1));
    }
}

/*
 * This function creates a link from the Network to a NI.
 * It creates a Network Link from a Router to the NI and
 * a Credit Link from NI to the Router
*/

void
GarnetNetwork::makeExtOutLink(SwitchID src, NodeID dest, BasicLink* link,
                             const NetDest& routing_table_entry)
{
    assert(dest < m_nodes);
    assert(src < m_routers.size());
    assert(m_routers[src] != NULL);

    GarnetExtLink* garnet_link = safe_cast<GarnetExtLink*>(link);

    // GarnetExtLink is bi-directional
    NetworkLink* net_link = garnet_link->m_network_links[LinkDirection_Out];
    net_link->setType(EXT_OUT_);
    CreditLink* credit_link = garnet_link->m_credit_links[LinkDirection_Out];

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    PortDirection src_outport_dirn = "Local";
    m_routers[src]->addOutPort(src_outport_dirn, net_link,
                               routing_table_entry,
                               link->m_weight, credit_link);
    m_nis[dest]->addInPort(net_link, credit_link);
}

/*
 * This function creates an internal network link between two routers.
 * It adds both the network link and an opposite credit link.
*/

void
GarnetNetwork::makeInternalLink(SwitchID src, SwitchID dest, BasicLink* link,
                                const NetDest& routing_table_entry,
                                PortDirection src_outport_dirn,
                                PortDirection dst_inport_dirn)
{
    GarnetIntLink* garnet_link = safe_cast<GarnetIntLink*>(link);

    // GarnetIntLink is unidirectional
    NetworkLink* net_link = garnet_link->m_network_link;
    net_link->setType(INT_);
    CreditLink* credit_link = garnet_link->m_credit_link;

    m_networklinks.push_back(net_link);
    m_creditlinks.push_back(credit_link);

    m_routers[dest]->addInPort(dst_inport_dirn, net_link, credit_link);
    m_routers[src]->addOutPort(src_outport_dirn, net_link,
                               routing_table_entry,
                               link->m_weight, credit_link);
}

// Total routers in the network
int
GarnetNetwork::getNumRouters()
{
    return m_routers.size();
}

// Get ID of router connected to a NI.
int
GarnetNetwork::get_router_id(int ni)
{
    return m_nis[ni]->get_router_id();
}

void
GarnetNetwork::regStats()
{
    Network::regStats();

    // Packets
    m_packets_received
        .init(m_virtual_networks)
        .name(name() + ".packets_received")
        .flags(Stats::pdf | Stats::total | Stats::nozero | Stats::oneline)
        ;

    m_packets_injected
        .init(m_virtual_networks)
        .name(name() + ".packets_injected")
        .flags(Stats::pdf | Stats::total | Stats::nozero | Stats::oneline)
        ;

    m_packet_network_latency
        .init(m_virtual_networks)
        .name(name() + ".packet_network_latency")
        .flags(Stats::oneline)
        ;

    m_packet_queueing_latency
        .init(m_virtual_networks)
        .name(name() + ".packet_queueing_latency")
        .flags(Stats::oneline)
        ;

    for (int i = 0; i < m_virtual_networks; i++) {
        m_packets_received.subname(i, csprintf("vnet-%i", i));
        m_packets_injected.subname(i, csprintf("vnet-%i", i));
        m_packet_network_latency.subname(i, csprintf("vnet-%i", i));
        m_packet_queueing_latency.subname(i, csprintf("vnet-%i", i));
    }

    m_avg_packet_vnet_latency
        .name(name() + ".average_packet_vnet_latency")
        .flags(Stats::oneline);
    m_avg_packet_vnet_latency =
        m_packet_network_latency / m_packets_received;

    m_avg_packet_vqueue_latency
        .name(name() + ".average_packet_vqueue_latency")
        .flags(Stats::oneline);
    m_avg_packet_vqueue_latency =
        m_packet_queueing_latency / m_packets_received;

    m_avg_packet_network_latency
        .name(name() + ".average_packet_network_latency");
    m_avg_packet_network_latency =
        sum(m_packet_network_latency) / sum(m_packets_received);

    m_avg_packet_queueing_latency
        .name(name() + ".average_packet_queueing_latency");
    m_avg_packet_queueing_latency
        = sum(m_packet_queueing_latency) / sum(m_packets_received);

    m_avg_packet_latency
        .name(name() + ".average_packet_latency");
    m_avg_packet_latency
        = m_avg_packet_network_latency + m_avg_packet_queueing_latency;

    // Flits
    m_flits_received
        .init(m_virtual_networks)
        .name(name() + ".flits_received")
        .flags(Stats::pdf | Stats::total | Stats::nozero | Stats::oneline)
        ;

    m_flits_injected
        .init(m_virtual_networks)
        .name(name() + ".flits_injected")
        .flags(Stats::pdf | Stats::total | Stats::nozero | Stats::oneline)
        ;

    m_flit_network_latency
        .init(m_virtual_networks)
        .name(name() + ".flit_network_latency")
        .flags(Stats::oneline)
        ;

    m_flit_queueing_latency
        .init(m_virtual_networks)
        .name(name() + ".flit_queueing_latency")
        .flags(Stats::oneline)
        ;

    for (int i = 0; i < m_virtual_networks; i++) {
        m_flits_received.subname(i, csprintf("vnet-%i", i));
        m_flits_injected.subname(i, csprintf("vnet-%i", i));
        m_flit_network_latency.subname(i, csprintf("vnet-%i", i));
        m_flit_queueing_latency.subname(i, csprintf("vnet-%i", i));
    }

    m_avg_flit_vnet_latency
        .name(name() + ".average_flit_vnet_latency")
        .flags(Stats::oneline);
    m_avg_flit_vnet_latency = m_flit_network_latency / m_flits_received;

    m_avg_flit_vqueue_latency
        .name(name() + ".average_flit_vqueue_latency")
        .flags(Stats::oneline);
    m_avg_flit_vqueue_latency =
        m_flit_queueing_latency / m_flits_received;

    m_avg_flit_network_latency
        .name(name() + ".average_flit_network_latency");
    m_avg_flit_network_latency =
        sum(m_flit_network_latency) / sum(m_flits_received);

    m_avg_flit_queueing_latency
        .name(name() + ".average_flit_queueing_latency");
    m_avg_flit_queueing_latency =
        sum(m_flit_queueing_latency) / sum(m_flits_received);

    m_avg_flit_latency
        .name(name() + ".average_flit_latency");
    m_avg_flit_latency =
        m_avg_flit_network_latency + m_avg_flit_queueing_latency;


    // Hops
    m_avg_hops.name(name() + ".average_hops");
    m_avg_hops = m_total_hops / sum(m_flits_received);

    // Links
    m_total_ext_in_link_utilization
        .name(name() + ".ext_in_link_utilization");
    m_total_ext_out_link_utilization
        .name(name() + ".ext_out_link_utilization");
    m_total_int_link_utilization
        .name(name() + ".int_link_utilization");
    m_average_link_utilization
        .name(name() + ".avg_link_utilization");

    m_average_vc_load
        .init(m_virtual_networks * m_vcs_per_vnet)
        .name(name() + ".avg_vc_load")
        .flags(Stats::pdf | Stats::total | Stats::nozero | Stats::oneline)
        ;

    //add for TG
    m_total_task_execution_time
        .name(name() + ".total_task_execution_time");

}

void
GarnetNetwork::collateStats()
{
    RubySystem *rs = params()->ruby_system;
    double time_delta = double(curCycle() - rs->getStartCycle());

    for (int i = 0; i < m_networklinks.size(); i++) {
        link_type type = m_networklinks[i]->getType();
        int activity = m_networklinks[i]->getLinkUtilization();

        if (type == EXT_IN_)
            m_total_ext_in_link_utilization += activity;
        else if (type == EXT_OUT_)
            m_total_ext_out_link_utilization += activity;
        else if (type == INT_)
            m_total_int_link_utilization += activity;

        m_average_link_utilization +=
            (double(activity) / time_delta);

        vector<unsigned int> vc_load = m_networklinks[i]->getVcLoad();
        for (int j = 0; j < vc_load.size(); j++) {
            m_average_vc_load[j] += ((double)vc_load[j] / time_delta);
        }
    }

    // Ask the routers to collate their statistics
    for (int i = 0; i < m_routers.size(); i++) {
        m_routers[i]->collateStats();
    }
}

void
GarnetNetwork::print(ostream& out) const
{
    out << "[GarnetNetwork]";
}

GarnetNetwork *
GarnetNetworkParams::create()
{
    return new GarnetNetwork(this);
}

uint32_t
GarnetNetwork::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;

    for (unsigned int i = 0; i < m_routers.size(); i++) {
        num_functional_writes += m_routers[i]->functionalWrite(pkt);
    }

    for (unsigned int i = 0; i < m_nis.size(); ++i) {
        num_functional_writes += m_nis[i]->functionalWrite(pkt);
    }

    for (unsigned int i = 0; i < m_networklinks.size(); ++i) {
        num_functional_writes += m_networklinks[i]->functionalWrite(pkt);
    }

    return num_functional_writes;
}

// 
vector<int> 
GarnetNetwork::get_ratio_token(int *iterations){
    // int *ratiolist = new int[m_num_application];
    vector<int> ratiolist;
    for(int i = 0; i < m_num_application; i++){
        // ratiolist[i] = iterations[i];
        ratiolist.push_back(iterations[i]);
    }
    int gcd_of_all = ratiolist[0];
    // sort(ratiolist, ratiolist + m_num_application, greater<int>());
    sort(ratiolist.begin(), ratiolist.end());
    for(int i = 0; i < m_num_application; i++){
        gcd_of_all = gcd(gcd_of_all, ratiolist[i]);
    }
    for(int i = 0; i < m_num_application; i++){
        ratiolist[i] = iterations[i]/gcd_of_all;
    }
    return ratiolist;
}

bool
GarnetNetwork::readApplicationConfig(std::string filename){

    FILE *fp=fopen(filename.c_str(), "r");
    if (fp == NULL){
        fatal("Error opening the task graph traffic file!");
        return false;
    }

    //read the first line of all application information
    fscanf(fp, "%d", &m_num_application);
    fscanf(fp, "%d", &m_total_execution_iterations);
    // read the filename and excution iterations of all application
    m_application_name = new string[m_num_application];
    m_applicaton_execution_iterations = new int[m_num_application];
    for (int i=0;i<m_num_application;i++){
        char app_name[256];
        fscanf(fp, "%s", app_name);
        m_application_name[i] = app_name;
        fscanf(fp, "%d", &m_applicaton_execution_iterations[i]);
    }
    fclose(fp);

    //calculate ratio token for apps
    vector<int> ratiolist = get_ratio_token(m_applicaton_execution_iterations);
    for (int i=0;i<m_nodes/2;i++){
        m_nis[i]->initializeFixedRatioToken(ratiolist);
    }
    return true;
}

//for task graph traffic
bool
GarnetNetwork::loadTraffic(std::string filename){

    m_num_task = new int[m_num_application];
    m_num_edge = new int[m_num_application];
    m_num_head_task = new int[m_num_application];

    FILE *fp;
    size_t separator = filename.rfind("/");
    //directory name with "/"
    std::string dir_name = filename.substr(0, separator+1);

    for (int k=0;k<m_num_application;k++){
        //the absolute path of the application file
        std::string app_filename = dir_name+m_application_name[k];

        fp = fopen(app_filename.c_str(), "r");
        if (fp == NULL){
            fatal("Error opening the %s.stp file!", app_filename.c_str());
            return false;
        }

        int v[10];
        float d[10];
        //read the first line
        //number of tasks, number of edges, number of PUs
        int trace_type;

        //get rid of headers
        char ts[1000];
        for (int i=0;i<15;i++)
        {
            fgets(ts,1000,fp);
        }

        fscanf(fp, "%d", &trace_type);assert(0==trace_type);
        fscanf(fp, "%d", &m_num_proc);
        fscanf(fp, "%d", &m_num_task[k]);
        fscanf(fp, "%d", &m_num_edge[k]);

        assert( m_num_task[k]>0 && m_num_edge[k]>0 && m_num_proc>0 &&\
        m_applicaton_execution_iterations[k]>0);
        //get head task information
        fscanf(fp, "%d", &m_num_head_task[k]);
        int head_task_id;
        for (int i=0; i<m_num_head_task[k]; i++){
            fscanf(fp, "%d", &head_task_id);
            head_task[k].push_back(head_task_id);
        }

        // read the next number of tasks lines: task info
        for (int i=0; i<m_num_task[k]; i++) {
            fscanf(fp, "%d", &v[0]);    //task id
            fscanf(fp, "%d", &v[1]);    //mapped proc id
            fscanf(fp, "%d", &v[2]);    //shedule sequence number
            fscanf(fp, "%f", &d[0]);    //mu & sigma for
            fscanf(fp, "%f", &d[1]);    //task execution time distribution

            // add the task to the processor
            GraphTask t;
            t.set_id(v[0]);
            t.set_proc_id(v[1]);
            t.set_schedule(v[2]);

            t.set_statistical_execution_time(d[0], d[1]);
            t.set_max_time(d[0]+2*d[1]);

            t.set_required_times(m_applicaton_execution_iterations[k]);
            //for multi-app
            t.set_app_idx(k);
            t.initial();

            int current_node_id = getNodeIdbyCoreId(v[1]);

            //head task not in the list
            // std::vector<int>::iterator it = std::find(head_task[k].begin(), head_task[k].end(), v[0]);
            // if (it!=head_task[k].end())
                m_nis[current_node_id]->add_task(k, t, false);
            // else
            //     m_nis[current_node_id]->add_task(k, t, true);

        }

        // read the next number of edges lines: communication info
            for (int i=0; i<m_num_edge[k]; i++){

                fscanf(fp, "%d", &v[0]);//edge id
                fscanf(fp, "%d", &v[1]);//src task id
                fscanf(fp, "%d", &v[2]);//dst task id
                fscanf(fp, "%d", &v[3]);//src proc id
                fscanf(fp, "%d", &v[4]);//dst proc id
                fscanf(fp, "%d", &v[5]);//out_memory_start_address
                fscanf(fp, "%d", &v[6]);//out_memory_size
                fscanf(fp, "%d", &v[7]);//in_memory_start_address
                fscanf(fp, "%d", &v[8]);//in_memory_size

                //mu & sigma for token size distribution
                fscanf(fp, "%f", &d[0]);
                fscanf(fp, "%f", &d[1]);
                //lambda for pk generation interval distribution
                fscanf(fp, "%f", &d[2]);

                // construct the edge
                GraphEdge e;
                e.set_id(v[0]);
                e.set_src_task_id(v[1]);
                e.set_dst_task_id(v[2]);
                e.set_src_proc_id(v[3]);
                e.set_dst_proc_id(v[4]);
                //Note Here! We just consider the size of the out memory for the source task
                //e.set_out_memory(v[5],v[6]);
                //e.set_out_memory(v[5],10);
                e.set_out_memory(v[5],m_out_mem_size);

                // if (v[6]==-1)
                //     e.set_out_memory(v[5],INT_MAX);
                // else
                //     e.set_out_memory(v[5],v[6]);

                //e.set_in_memory(v[7],v[8]);
                e.set_in_memory(v[7],m_in_mem_size);


                e.set_statistical_token_size(d[0], d[1]);
                e.set_max_token_size(d[0]+2*d[1]);
                e.set_statistical_pkt_interval(d[2]);

                e.set_app_idx(k);
                e.initial();

                int src_node_id = getNodeIdbyCoreId(e.get_src_proc_id());
                GraphTask &src_task = m_nis[src_node_id]->\
                    get_task_by_task_id(e.get_src_proc_id(), \
                    k, e.get_src_task_id());

                int dst_node_id = getNodeIdbyCoreId(e.get_dst_proc_id());
                GraphTask &dst_task = m_nis[dst_node_id]->\
                    get_task_by_task_id(e.get_dst_proc_id(), k, \
                    e.get_dst_task_id());

                src_task.add_outgoing_edge(e);
                dst_task.add_incoming_edge(e);

                // Set vc_choice based on m_vc_allocation_object and node ID and vc_allocation_object_position
                int vc_choice;
                if(m_vc_allocation_object != " " && m_vcs_for_allocation > 0){
                    bool is_for_object = false;
                    int num_object = vc_allocation_object_position.size();
                    for (int i = 0; i < num_object; i++){
                        if((src_node_id == vc_allocation_object_position[i])||(dst_node_id \
                        == vc_allocation_object_position[i])){
                            vc_choice = (dst_node_id >= src_node_id);
                            is_for_object = true;
                            break;
                        }
                    }
                    if(is_for_object == false){
                        vc_choice = (dst_node_id >= src_node_id) + 2;
                    }
                }
                else if(m_vc_allocation_object == " " && m_vcs_for_allocation > 0){
                    fatal("vc_allocation_object is not assigned! vcs_for_allocation can not be positive!");
                }
                else{
                    vc_choice = (dst_node_id >= src_node_id);
                }
                e.set_vc_choice(vc_choice);
            }

        for (int i=0; i < m_nodes/2; i++) {
            m_nis[i]->sort_task_list();
        }

        fclose(fp);
    }

    unsigned int sum=0;
    printf("**********************\n");
    printf("**********************\n");
    printf("Traffic Information\n");
    for (int i=0; i < m_nodes /2 ; i++){
        int num_cores_in_node = m_nis[i]->get_num_cores();
        printf("**********************\n");
        printf("Node %d with %d Cores\n",\
            m_nis[i]->get_ni_id(), num_cores_in_node);
        printf("**********************\n");

        for (int j=0;j<num_cores_in_node;j++){
            int core_id = m_nis[i]->get_core_id_by_index(j);
            printf("Core Index: %5d\tCore Id: %5d\tCore Name: %7s\n", \
                j, m_nis[i]->get_core_id_by_index(j), \
                m_nis[i]->get_core_name_by_index(j).c_str());
            for (int ii=0;ii<m_num_application;ii++){
                int task_list_len = m_nis[i]->get_task_list_length(j, ii);
                printf("\n");
                printf("\tApplication: %s\n\n",m_application_name[ii].c_str());
                for (int k=0;k<task_list_len;k++){
                    GraphTask &t = m_nis[i]->get_task_by_offset(core_id,ii,k);
                    printf("  \tTask %5d\tshedule %5d\n",\
                        t.get_id(), t.get_schedule());
                }
                sum = sum + task_list_len;
            }
        }
        printf("\n");
    }

    printf("**********************\n");
    printf("Head Task\n");
    printf("**********************\n");
    for (int i=0;i<m_num_application;i++){
        printf("Application: %s\n\n",m_application_name[i].c_str());
        for (int j=0;j<m_num_head_task[i];j++){
            printf("\tTask ID: %5d\n", head_task[i][j]);
        }
    }
    printf("\n");
    printf("The Total task is %d\n\n", sum);

    int verify_task_sum=0;
    for (int i=0;i<m_num_application;i++)
        verify_task_sum += m_num_task[i];
    assert(sum==verify_task_sum);

    //Core[0] task schdule
    /*
    for (unsigned j=0; j<m_nis[0]->get_task_list_length(); j++){
            DPRINTF(TaskGraph, "Task %d shedule %d\n", m_nis[0]->\
            get_task_by_offset(j).get_id(),m_nis[0]->\
            get_task_by_offset(j).get_schedule());
        }
    */

    return true;
}

void
GarnetNetwork::wakeup(){
    if (isTaskGraphEnabled()){

        if (curCycle()%10000==0){
            *(throughput_info->stream())<<curCycle()<<"\t"<<current_execution_iterations[0]<<"\t"<<\
                double(current_execution_iterations[0])*1000000000/curCycle()<<endl;
        }

        if (! checkApplicationFinish())
        //each cycle would check finish
            scheduleEvent(Cycles(1));
        else {
            //collect simulation data
            PrintAppDelay();
            PrintTaskWaitingInfo();
/*
            for (int i = 0; i < m_nodes / 2; i++)
            {
                int num_cores_in_node = m_nis[i]->get_num_cores();
                for (int j = 0; j < num_cores_in_node; j++)
                {
                    int core_id = m_nis[i]->get_core_id_by_index(j);
                        for (int app_idx = 0; app_idx < m_num_application; app_idx++)
                        {
                            int task_list_len = m_nis[i]->get_task_list_length(j, app_idx);
                            if (task_list_len == 0)
                                continue;
                            string core_name = m_nis[i]->get_core_name_by_index(j);
                            printf("Core [%5d] : %s\n", core_id, core_name.c_str());
                            for (int k = 0; k < task_list_len; k++)
                            {
                                GraphTask &temp_task = m_nis[i]->get_task_by_offset(core_id, app_idx, k);
                                printf("\tTask [%3d] Completed : %d\n", temp_task.get_id(), temp_task.get_completed_times());
                            }
                        }
                    
                }
            }

            for (int i = 0; i < m_nodes / 2; i++)
            {
                int num_cores_in_node = m_nis[i]->get_num_cores();
                for (int j = 0; j < num_cores_in_node; j++)
                {
                    string core_name = m_nis[i]->get_core_name_by_index(j);
                    int core_buffer_size = m_nis[i]->get_core_buffer_size(j);
                    printf("Core Name %10s\tBuffer Size %10d\tBuffer Sent %10d\n", core_name.c_str(), core_buffer_size, m_nis[i]->core_buffer_sent[j]);
                }
            }
            printf("\n");
*/
            simout.close(task_start_time_vs_id);
            simout.close(task_start_end_time_vs_id);
            simout.close(task_start_time_vs_id_iters);
            simout.close(throughput_info);
            simout.close(app_delay_running_info);
            // simout.close(start_time_info);
            // simout.close(end_time_info);
            // simout.close(ete_info);
            simout.close(network_performance_info);
            simout.close(task_waiting_time_info);

            exitSimLoop("Network Task Graph Simulation Complete.");
        }
    }
}

void
GarnetNetwork::scheduleWakeupAbsolute(Cycles time){
    scheduleEventAbsolute(time);
}

bool
GarnetNetwork::checkApplicationFinish(){
/*
    if (curCycle() % 20000 == 0)
    {
        for (int i = 0; i < m_nodes / 2; i++)
        {
            int num_cores_in_node = m_nis[i]->get_num_cores();
            for (int j = 0; j < num_cores_in_node; j++)
            {
                int core_id = m_nis[i]->get_core_id_by_index(j);
                for (int app_idx = 0; app_idx < m_num_application; app_idx++)
                {
                    int task_list_len = m_nis[i]->get_task_list_length(j, app_idx);
                    if (task_list_len == 0)
                        continue;
                    string core_name = m_nis[i]->get_core_name_by_index(j);
                    printf("Core [%5d] : %s\n", core_id, core_name.c_str());
                    for (int k = 0; k < task_list_len; k++)
                    {
                        GraphTask &temp_task = m_nis[i]->get_task_by_offset(core_id, app_idx, k);
                        printf("\tTask [%3d] Completed : %d\n", temp_task.get_id(), temp_task.get_completed_times());
                    }
                }
            }
        }
        printf("\n\n");
    }
*/
/*
    if (curCycle()%1000==0){
    for (int i=0;i<m_nodes/2;i++){
        int num_cores_in_node = m_nis[i]->get_num_cores();
        for (int j=0;j<num_cores_in_node;j++){
            int core_id = m_nis[i]->get_core_id_by_index(j);            
            if(core_id==12){
            for (int app_idx=0;app_idx<m_num_application;app_idx++){
                int task_list_len = m_nis[i]->get_task_list_length(j, app_idx);
                if (task_list_len==0)    continue;
                string core_name = m_nis[i]->get_core_name_by_index(j);
                printf("Core [%5d] : %s\n", core_id, core_name.c_str());
                for (int k=0;k<task_list_len;k++){
                GraphTask& temp_task = m_nis[i]->\
                    get_task_by_offset(core_id, app_idx, k);
                    if (temp_task.get_completed_times()>=\
                    temp_task.get_required_times())
                    printf("\tCompleted Task [%3d]\n", temp_task.get_id());
                    else
                    printf("\tNot Completed Task [%3d]\n", temp_task.get_id());
                }
            }
            }
        }
    }
    }
*/
/*
    if (curCycle()%20000==0){
        for (int i=0;i<m_nodes/2;i++){
        int num_cores_in_node = m_nis[i]->get_num_cores();
        for (int j=0;j<num_cores_in_node;j++){
            string core_name = m_nis[i]->get_core_name_by_index(j);
            int core_buffer_size = m_nis[i]->get_core_buffer_size(j);
            printf("Core Name %10s\tBuffer Size %10d\tBuffer Sent %10d\n", core_name.c_str(), core_buffer_size, m_nis[i]->core_buffer_sent[j]);
        }
        }
        printf("\n\n");
    }
*/
    for(int i=0;i<m_num_application;i++){
        if(current_execution_iterations[i]==m_applicaton_execution_iterations[i]){
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool
GarnetNetwork::constructArchitecture(std::string filename){

    FILE *fp=fopen(filename.c_str(), "r");
    if (fp == NULL){
        fatal("Error opening the architecture file!");
        return false;
    }

    int num_nodes;//total nodes in soc
    fscanf(fp, "%d", &num_nodes);
    assert(num_nodes==m_nodes/2);

    int node_id;
    int num_cores_in_node;
    int sum_cores = 0;
    for (int i=0;i<num_nodes;i++){
        fscanf(fp, "%d", &node_id);
        fscanf(fp, "%d", &num_cores_in_node);

        int* core_id = new int[num_cores_in_node];
        std::string* core_name = new std::string[num_cores_in_node];
        int* core_thread = new int[num_cores_in_node];
        sum_cores += num_cores_in_node;

        for (int j=0;j<num_cores_in_node;j++){
            char* _core_name = new char;

            fscanf(fp, "%d", &core_id[j]);
            fscanf(fp, "%s", _core_name);
            core_name[j] = _core_name;
            fscanf(fp, "%d", &core_thread[j]);

            // If user enter the object name then find their id
            if(m_vc_allocation_object != " "){
                std::string::size_type idx;
                idx=core_name[j].find(m_vc_allocation_object);
                if (idx != string::npos){ 
                    vc_allocation_object_position.push_back(node_id);
                }
            }
            //record PE-7 position for initial task judgement in NI
            std::string::size_type idx;
            if(string::npos != core_name[j].find("PE-7")){
                entrance_NI = node_id;
                entrance_core = core_id[j];
                entrance_idx_in_NI = j;
            }
            idx=core_name[j].find(m_vc_allocation_object);

            m_core_id_node_id.insert(make_pair(core_id[j], node_id));

            delete _core_name;
        }

        if (!m_nis[node_id]->configureNode(num_cores_in_node, \
            core_id, core_name, core_thread, m_num_application))
            return false;

        delete [] core_id;
        delete [] core_name;
        delete [] core_thread;
    }

    assert(m_core_id_node_id.size()==sum_cores);
    m_num_core = sum_cores;

    //print core map to node
    printf("**********************\n");
    printf("Core ID -> Node ID\n");
    cout<<"\n";
    for (map<int,int>::iterator iter=m_core_id_node_id.begin();\
        iter!=m_core_id_node_id.end();iter++)
        cout<<iter->first<<"\t"<<iter->second<<"\n";
    cout<<endl;
    printf("**********************\n");

    return true;
}

// because of cluster, one node with several cores
int
GarnetNetwork::getNodeIdbyCoreId(int core_id){
    map<int, int>::iterator iter = m_core_id_node_id.find(core_id);
    if (iter == m_core_id_node_id.end())
        fatal("GarnetNetwork: Error in finding Node Id by Core Id !");
    else
        return(m_core_id_node_id[core_id]);
}

// print each ete delay for log
void
GarnetNetwork::PrintAppDelay(){
    for (int app_idx=0;app_idx<m_num_application;app_idx++){
        int Average_ETE_delay=0;
        for (int i=0;i<m_applicaton_execution_iterations[app_idx];i++){
/*           
            if (m_print_task_execution_info)
              *(task_start_time_vs_id_iters->stream())<<"Execution\n";
            int max_time=-1;
            int min_time=INT_MAX;
            int max_time_core_id=-1;
            int max_time_task_id=-1;
            int min_time_core_id=-1;
            int min_time_task_id=-1;
            for (int j=0;j<m_nodes/2;j++){
                int num_cores_in_node = m_nis[j]->get_num_cores();
                for (int k=0;k<num_cores_in_node;k++){
                    int task_list_len = m_nis[j]->\
                        get_task_list_length(k, app_idx);
                    int core_id = m_nis[j]->get_core_id_by_index(k);
                    for (int l=0;l<task_list_len;l++){
                        GraphTask& temp_task = m_nis[j]->\
                            get_task_by_offset(core_id, app_idx, l);
                        if (min_time > temp_task.get_start_time(i)){
                            min_time_core_id = core_id;
                            min_time_task_id = temp_task.get_id();
                        }
                        if (max_time < temp_task.get_end_time(i)){
                            max_time_core_id = core_id;
                            max_time_task_id = temp_task.get_id();
                        }
                        min_time = min(temp_task.get_start_time(i), min_time);
                        max_time = max(temp_task.get_end_time(i), max_time);
                        if (m_print_task_execution_info)
                            *(task_start_time_vs_id_iters->stream())<<\
                            temp_task.get_start_time(i)<<"\t"<<\
                            core_id<<"\t"<<temp_task.get_id()<<"\n";
                    }
                }
            }
            if (m_print_task_execution_info){
                *(task_start_end_time_vs_id->stream())<<"min\t"<<\
                    min_time<<"\t"<<min_time_core_id<<"\t"\
                    <<min_time_task_id<<"\n";
                *(task_start_end_time_vs_id->stream())<<"max\t"<<\
                    max_time<<"\t"<<max_time_core_id<<"\t"\
                    <<max_time_task_id<<"\n";
            }
*/
            Average_ETE_delay=Average_ETE_delay+ETE_delay[app_idx][i];
        }
        Average_ETE_delay = Average_ETE_delay / m_applicaton_execution_iterations[app_idx];

        cout<<"info: Application - "<<m_application_name[app_idx]<<" - has executed successfully !\n";
        printf("Execution iterations: %3d\n", m_applicaton_execution_iterations[app_idx]);
        printf("Average Iteration Delay: %d\n", Average_ETE_delay);

        for (int i=0; i<m_applicaton_execution_iterations[app_idx];i++){
            int s=task_start_time[app_idx][i];
            int e=task_end_time[app_idx][i];
            printf("\tIteration %3d \tApplication Start time %10d \t\
            Application End time %10d \t Applcation Execution Delay: \
            %d\n", i, s, e, ETE_delay[app_idx][i]);
        }
    }
}

void
GarnetNetwork::PrintTaskWaitingInfo(){

    int **node_waiting_time;
    int **core_waiting_time;
    string *core_waiting_name;
    int *total_node_waiting_time;
    int *total_core_waiting_time;

    //new
    node_waiting_time = new int* [m_num_application];
    core_waiting_time = new int* [m_num_application];
    core_waiting_name = new string [m_num_core];
    total_core_waiting_time = new int [m_num_core];
    total_node_waiting_time = new int [m_nodes/2];
    //initial
    for (int i=0;i<m_num_application;i++){
        core_waiting_time[i] = new int [m_num_core];
        node_waiting_time[i] = new int [m_nodes/2];
    }

    for (int i=0;i<m_num_application;i++){
        for (int j=0;j<m_num_core;j++)
            core_waiting_time[i][j] = 0;

        for (int j=0;j<m_nodes/2;j++)
            node_waiting_time[i][j] = 0;
    }

    for (int j=0;j<m_num_core;j++)
        total_core_waiting_time[j] = 0;

    for (int j=0;j<m_nodes/2;j++)
        total_node_waiting_time[j] = 0;
    //compute
    for (int app_idx = 0; app_idx < m_num_application; app_idx++){
        for (int j = 0; j < m_nodes / 2; j++){

            int num_cores_in_node = m_nis[j]->get_num_cores();
            int node_task_waiting_time = 0;

            for (int k = 0; k < num_cores_in_node; k++){

                int task_list_len = m_nis[j]->get_task_list_length(k, app_idx);
                int core_id = m_nis[j]->get_core_id_by_index(k);
                string core_name = m_nis[j]->get_core_name_by_index(k);
                int core_task_waiting_time = 0;

                for (int l = 0; l < task_list_len; l++){

                    GraphTask &temp_task = m_nis[j]->get_task_by_offset(core_id, app_idx, l);
                    for (int m = 0; m < m_applicaton_execution_iterations[app_idx]; m++)
                        core_task_waiting_time += temp_task.get_task_waiting_time(m);
                }

                core_waiting_time[app_idx][core_id] = core_task_waiting_time;
                core_waiting_name[core_id] = core_name;
                node_task_waiting_time += core_task_waiting_time;
                //for the core, ignore the app_idx
                total_core_waiting_time[core_id] += core_task_waiting_time;
            }

            node_waiting_time[app_idx][j] = node_task_waiting_time;
            //for the node, ignore the app_idx
            total_node_waiting_time[j] += node_task_waiting_time;
        }
    }

    //******Print to Log******
    for (int app_idx = 0; app_idx < m_num_application; app_idx++){

        *(task_waiting_time_info->stream()) << "Application - " << m_application_name[app_idx] \
            << "\nCore_Id\tCore_Name\tTask_Waiting_Time\n";
        for (int i=0;i<m_num_core;i++){
            *(task_waiting_time_info->stream()) << setw(7) << i << "\t" << setw(9) << core_waiting_name[i]  \
                << "\t" << setw(17) << core_waiting_time[app_idx][i] << "\n";
        }

        *(task_waiting_time_info->stream()) <<"\nNode_Id\tTask_Waiting_Time\tAll_Core_Id\n";
        for (int i=0;i<m_nodes/2;i++){
            *(task_waiting_time_info->stream()) << setw(7) << i  << "\t" << setw(17) << node_waiting_time[app_idx][i] << "\t\t";
            int num_cores_in_node = m_nis[i]->get_num_cores();
            for (int j=0;j<num_cores_in_node;j++){
                int core_id = m_nis[i]->get_core_id_by_index(j);
                *(task_waiting_time_info->stream()) << core_id << " ";
            }
            *(task_waiting_time_info->stream()) << "\n";
        }
        *(task_waiting_time_info->stream()) << "\n";
    }

    *(task_waiting_time_info->stream()) << "Total_Task_Waiting_Time\nCore_Id\tCore_Name\tTask_Waiting_Time\n";
    for (int i=0;i<m_num_core;i++){
        *(task_waiting_time_info->stream()) << setw(7) << i << "\t" << setw(9) << core_waiting_name[i]  \
            << "\t" << setw(17) << total_core_waiting_time[i] << "\n";
    }

    *(task_waiting_time_info->stream()) <<"\nNode_Id\tTask_Waiting_Time\tAll_Core_Id\n";
    for (int i=0;i<m_nodes/2;i++){
        *(task_waiting_time_info->stream()) << setw(7) << i  << "\t" << setw(17) << total_node_waiting_time[i] << "\t\t";
        int num_cores_in_node = m_nis[i]->get_num_cores();
        for (int j=0;j<num_cores_in_node;j++){
            int core_id = m_nis[i]->get_core_id_by_index(j);
            *(task_waiting_time_info->stream()) << core_id << " ";
        }
        *(task_waiting_time_info->stream()) << "\n";
    }

    for (int i=0;i<m_num_application;i++){
        delete [] core_waiting_time[i];
        delete [] node_waiting_time[i];
    }
    delete [] core_waiting_time;
    delete [] node_waiting_time;
    delete [] core_waiting_name;
    delete [] total_core_waiting_time;
    delete [] total_node_waiting_time;
}


void
GarnetNetwork::output_ete_delay(int app_idx, int ex_iters)
{
    ETE_delay[app_idx][ex_iters] = task_end_time[app_idx][ex_iters] - task_start_time[app_idx][ex_iters];

    *(app_delay_running_info->stream())<<m_application_name[app_idx]<<"\t"<<ex_iters<<"\t"<<task_start_time[app_idx][ex_iters]<<\
        "\t"<<task_end_time[app_idx][ex_iters]<<"\t"<<ETE_delay[app_idx][ex_iters]<<endl;

    *(network_performance_info->stream())<<m_application_name[app_idx]<<"\t"<<ex_iters<<"\t"<<m_avg_flit_latency.total()<<"\t"<<m_avg_flit_network_latency.total()<<\
        "\t"<<m_avg_flit_queueing_latency.total()<<"\t"<<m_flits_received.total()<<"\t"<<m_avg_hops.total()<<endl;
    
    // *(start_time_info->stream())<<task_start_time[app_idx][ex_iters]<<endl;
    // *(end_time_info->stream())<<task_end_time[app_idx][ex_iters]<<endl;
    // *(ete_info->stream())<<ETE_delay[app_idx][ex_iters]<<endl;
}

bool
GarnetNetwork::back_pressure(int m_id){
    /*
    int num_buffer = m_nis[m_id]->get_num_tokens();
    if (num_buffer>20){
        return true;
    }else if (num_buffer == -2){
        fatal("Wrong Back Pressure !");
    }else{
        return false;
    }
    */
    return false;
}

void
GarnetNetwork::update_in_memory_info(int core_id, int app_idx, int src_task_id, int edge_id){
    int node_id = getNodeIdbyCoreId(core_id);
    GraphTask &src_task = m_nis[node_id]->get_task_by_task_id(core_id, app_idx, src_task_id);
    GraphEdge &out_edge = src_task.get_outgoing_edge_by_eid(edge_id);

    assert(out_edge.update_in_memory_read_pointer());
}

