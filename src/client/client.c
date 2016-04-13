#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>

#include "../common/common.h"
#include "../common/cdf.h"
#include "../common/conn.h"

#ifndef max
    #define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
    #define min(a,b) ((a) < (b) ? (a) : (b))
#endif

bool verbose_mode = false; //by default, we don't give more detailed output

char config_file_name[80] = {'\0'}; //configuration file name
char dist_file_name[80] = {'\0'};   //size distribution file name
char fct_log_name[80] = "flows.txt";    //default log file name
char result_script_name[80] = {'\0'};   //name of script file to parse final results
int seed = 0; //random seed
unsigned int usleep_overhead_us = 0; //usleep overhead
struct timeval tv_start, tv_end; //start and end time of traffic
unsigned int num_new_conn = 0; //new established connections

/* per-server variables */
unsigned int num_server = 0; //total number of servers
unsigned int *server_port = NULL;   //ports of servers
char (*server_addr)[20] = NULL;    //IP addresses of servers
unsigned int *server_req_count = NULL;   //the number flows generated by each server

unsigned int num_dscp = 0; //Number of DSCP
unsigned int *dscp_value = NULL;
unsigned int *dscp_prob = NULL;
unsigned int dscp_prob_total = 0;

unsigned int num_rate = 0; //Number of sending rates
unsigned int *rate_value = NULL;
unsigned int *rate_prob = NULL;
unsigned int rate_prob_total = 0;

double load = -1; //Network load (Mbps)
unsigned int req_total_num = 0; //Total number of requests to generate
unsigned int req_total_time = 0; //Total time to generate requests (in seconds)
struct CDF_Table* req_size_dist;
unsigned int period_us;  //Average request arrival interval (us)

/* per-request variables */
unsigned int *req_size = NULL;   //flow size
unsigned int *req_server_id = NULL;  //server ID
unsigned int *req_dscp = NULL;   //DSCP of flow
unsigned int *req_rate = NULL;   //sending rate of flow
unsigned int *req_sleep_us = NULL; //sleep time interval
struct timeval *req_start_time; //start time of flow
struct timeval *req_stop_time;  //stop time of flow

struct Conn_List* connection_lists = NULL; //connection pool

/* Print usage of the program */
void print_usage(char *program);
/* Read command line arguments */
void read_args(int argc, char *argv[]);
/* Read configuration file */
void read_config(char *file_name);
/* Set request variables */
void set_req_variables();
/* Receive traffic from established connections */
void *listen_connection(void *ptr);
/* Generate flow requests */
void run_requests();
/* Generate a flow request to the server */
void run_request(unsigned int req_id);
/* Terminate all existing connections */
void exit_connections();
/* Terminate a connection */
void exit_connection(struct Conn_Node* node);
/* Print statistic data */
void print_statistic();
/* Clean up resources */
void cleanup();

int main(int argc, char *argv[])
{
    unsigned int i = 0;
    struct Conn_Node* ptr = NULL;

    /* read program arguments */
    read_args(argc, argv);

    /* set seed value for random number generation */
    if (seed == 0)
    {
        gettimeofday(&tv_start, NULL);
        srand((tv_start.tv_sec*1000000) + tv_start.tv_usec);
    }
    else
        srand(seed);

    /* read configuration file */
    read_config(config_file_name);
    /* set request variables */
    set_req_variables();

    /* Calculate usleep overhead */
    usleep_overhead_us = get_usleep_overhead(20);
    if (verbose_mode)
    {
        printf("===========================================\n");
        printf("The usleep overhead is %u us\n", usleep_overhead_us);
        printf("===========================================\n");
    }

    /* We use calloc here to implicitly initialize struct Conn_List as 0 */
    connection_lists = (struct Conn_List*)calloc(num_server, sizeof(struct Conn_List));
    if (!connection_lists)
    {
        cleanup();
        error("Error: calloc");
    }

    /* Initialize connection pool and establish connections to servers */
    for (i = 0; i < num_server; i++)
    {
        /* Initialize server IP and port information */
        if (!Init_Conn_List(&connection_lists[i], i, server_addr[i], server_port[i]))
        {
            cleanup();
            error("Error: Init_Conn_List");
        }
        /* Establish TG_PAIR_INIT_CONN connections to server_addr[i]:server_port[i] */
        if (!Insert_Conn_List(&connection_lists[i], TG_PAIR_INIT_CONN))
        {
            cleanup();
            error("Error: Insert_Conn_List");
        }
        //Print_Conn_List(&connection_lists[i]);
    }

    /* Start threads to receive traffic */
    for (i = 0; i < num_server; i++)
    {
        ptr = connection_lists[i].head;
        while (true)
        {
            if (!ptr)
                break;
            else
            {
                pthread_create(&(ptr->thread), NULL, listen_connection, (void*)ptr);
                ptr = ptr->next;
            }
        }
    }

    printf("===========================================\n");
    printf("Start to generate requests\n");
    printf("===========================================\n");
    gettimeofday(&tv_start, NULL);
    run_requests();
    gettimeofday(&tv_end, NULL);

    /* Close existing connections */
    printf("===========================================\n");
    printf("Exit connections\n");
    printf("===========================================\n");
    exit_connections();

    printf("===========================================\n");
    for (i = 0; i < num_server; i++)
        Print_Conn_List(&connection_lists[i]);
    printf("===========================================\n");
    print_statistic();

    /* Release resources */
    cleanup();

    /* Parse results */
    printf("===========================================\n");
    printf("Flow completion times (FCT) results\n");
    printf("===========================================\n");
    if (strlen(result_script_name) > 0)
    {
        char cmd[180] = {'\0'};
        sprintf(cmd, "python %s %s", result_script_name, fct_log_name);
        system(cmd);
    }

    return 0;
}

/* Print usage of the program */
void print_usage(char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("-b <bandwidth>  expected average RX bandwidth in Mbits/sec\n");
    printf("-c <file>       configuration file (required)\n");
    printf("-n <number>     number of requests (instead of -t)\n");
    printf("-t <time>       time in seconds (instead of -n)\n");
    printf("-l <file>       log file with flow completion times (default %s)\n", fct_log_name);
    printf("-s <seed>       seed to generate random numbers (default current time)\n");
    printf("-r <file>       python script to parse result files\n");
    printf("-v              give more detailed output (verbose)\n");
    printf("-h              display help information\n");
}

/* Read command line arguments */
void read_args(int argc, char *argv[])
{
    int i = 1;
    bool error = false;

    if (argc == 1)
    {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
    }

    while (i < argc)
    {
        if (strlen(argv[i]) == 2 && strcmp(argv[i], "-b") == 0)
        {
            if (i+1 < argc)
            {
                load = atof(argv[i+1]);
                if (load <= 0)
                {
                    printf("Invalid average RX bandwidth: %f\n", load);
                    print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                i += 2;
            }
            else
            {
                printf("Cannot read average RX bandwidth\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-c") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) < sizeof(config_file_name))
            {
                sprintf(config_file_name, "%s", argv[i+1]);
                i += 2;
            }
            else
            {
                printf("Cannot read configuration file name\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-n") == 0)
        {
            if (i+1 < argc)
            {
                req_total_num = (unsigned int)strtoul(argv[i+1], NULL, 10);
                i += 2;
            }
            else
            {
                printf("Cannot read number of requests\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-t") == 0)
        {
            if (i+1 < argc)
            {
                req_total_time = (unsigned int)strtoul(argv[i+1], NULL, 10);
                i += 2;
            }
            else
            {
                printf("Cannot read time to generate requests\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-l") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) < sizeof(fct_log_name))
            {
                sprintf(fct_log_name, "%s", argv[i+1]);
                i += 2;
            }
            else
            {
                printf("Cannot read log file name\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-s") == 0)
        {
            if (i+1 < argc)
            {
                seed = atoi(argv[i+1]);
                i += 2;
            }
            else
            {
                printf("Cannot read seed value\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-r") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) < sizeof(result_script_name))
            {
                sprintf(result_script_name, "%s", argv[i+1]);
                i += 2;
            }
            else
            {
                printf("Cannot read script file name\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-v") == 0)
        {
            verbose_mode = true;
            i++;
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else
        {
            printf("Invalid option %s\n", argv[i]);
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (load < 0)
    {
        printf("You need to specify the average RX bandwidth (-b)\n");
        error = true;
    }

    if (req_total_num == 0 && req_total_time == 0)
    {
        printf("You need to specify either the number of requests (-n) or the time to generate requests (-t)\n");
        error = true;
    }
    else if (req_total_num > 0 && req_total_time > 0)
    {
        printf("You cannot specify both the number of requests (-n) and the time to generate requests (-t)\n");
        error = true;
    }

    if (error)  //Invalid arguments
    {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
}

/* Read configuration file */
void read_config(char *file_name)
{
    FILE *fd = NULL;
    char line[256] = {'\0'};
    char key[80] = {'\0'};
    num_server = 0;    //Number of senders
    unsigned int num_dist = 0;   //Number of flow size distributions
    num_dscp = 0; //Number of DSCP (optional)
    num_rate = 0; //Number of sending rates (optional)

    printf("===========================================\n");
    printf("Reading configuration file %s\n", file_name);
    printf("===========================================\n");

    /* Parse configuration file for the first time */
    fd = fopen(file_name, "r");
    if (!fd)
        error("Error: fopen");

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        sscanf(line, "%s", key);
        if (!strcmp(key, "server"))
            num_server++;
        else if (!strcmp(key, "req_size_dist"))
            num_dist++;
        else if (!strcmp(key, "dscp"))
            num_dscp++;
        else if (!strcmp(key, "rate"))
            num_rate++;
    }

    fclose(fd);

    if (num_server < 1)
        error("Error: configuration file should provide at least one server");
    if (num_dist != 1)
        error("Error: configuration file should provide one request size distribution");

    /* Initialize configuration */
    /* per-server variables*/
    server_port = (unsigned int*)calloc(num_server, sizeof(unsigned int));
    server_addr = (char (*)[20])calloc(num_server, sizeof(char[20]));
    server_req_count = (unsigned int*)calloc(num_server, sizeof(unsigned int));  //initialize as 0
    /* DSCP and probability */
    dscp_value = (unsigned int*)calloc(max(num_dscp, 1), sizeof(unsigned int));
    dscp_prob = (unsigned int*)calloc(max(num_dscp, 1), sizeof(unsigned int));
    /* sending rate value and probability */
    rate_value = (unsigned int*)calloc(max(num_rate, 1), sizeof(unsigned int));
    rate_prob = (unsigned int*)calloc(max(num_rate, 1), sizeof(unsigned int));

    if (!server_port || !server_addr || !server_req_count || !dscp_value || !dscp_prob || !rate_value || !rate_prob)
    {
        cleanup();
        error("Error: calloc");
    }

    /* Second time */
    num_server = 0;
    num_dscp = 0;
    num_rate = 0;

    fd = fopen(file_name, "r");
    if (!fd)
    {
        cleanup();
        error("Error: fopen");
    }

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        remove_newline(line);
        sscanf(line, "%s", key);

        if (!strcmp(key, "server"))
        {
            sscanf(line, "%s %s %u", key, server_addr[num_server], &server_port[num_server]);
            if (verbose_mode)
                printf("Server[%u]: %s, Port: %u\n", num_server, server_addr[num_server], server_port[num_server]);
            num_server++;
        }
        else if (!strcmp(key, "req_size_dist"))
        {
            sscanf(line, "%s %s", key, dist_file_name);
            if (verbose_mode)
                printf("Loading request size distribution: %s\n", dist_file_name);

            req_size_dist = (struct CDF_Table*)malloc(sizeof(struct CDF_Table));
            if (!req_size_dist)
            {
                cleanup();
                error("Error: malloc");
            }

            init_CDF(req_size_dist);
            load_CDF(req_size_dist, dist_file_name);
            if (verbose_mode)
            {
                printf("===========================================\n");
                print_CDF(req_size_dist);
                printf("Average request size: %.2f bytes\n", avg_CDF(req_size_dist));
                printf("===========================================\n");
            }
        }
        else if (!strcmp(key, "dscp"))
        {
            sscanf(line, "%s %u %u", key, &dscp_value[num_dscp], &dscp_prob[num_dscp]);
            if (dscp_value[num_dscp] < 0 || dscp_value[num_dscp] >= 64)
            {
                cleanup();
                error("Invalid DSCP value");
            }
            else if (dscp_prob[num_dscp] < 0)
            {
                cleanup();
                error("Invalid DSCP probability value");
            }
            dscp_prob_total += dscp_prob[num_dscp];
            if (verbose_mode)
                printf("DSCP: %u, Prob: %u\n", dscp_value[num_dscp], dscp_prob[num_dscp]);
            num_dscp++;
        }
        else if (!strcmp(key, "rate"))
        {
            sscanf(line, "%s %uMbps %u", key, &rate_value[num_rate], &rate_prob[num_rate]);
            if (rate_value[num_rate] < 0)
            {
                cleanup();
                error("Invalid sending rate value");
            }
            else if (rate_prob[num_rate] < 0)
            {
                cleanup();
                error("Invalid sending rate probability value");
            }
            rate_prob_total += rate_prob[num_rate];
            if (verbose_mode)
                printf("Rate: %uMbps, Prob: %u\n", rate_value[num_rate], rate_prob[num_rate]);
            num_rate++;
        }
    }

    fclose(fd);

    /* By default, DSCP value is 0 */
    if (num_dscp == 0)
    {
        num_dscp = 1;
        dscp_value[0] = 0;
        dscp_prob[0] = 100;
        dscp_prob_total = dscp_prob[0];
        if (verbose_mode)
            printf("DSCP: %u, Prob: %u\n", dscp_value[0], dscp_prob[0]);
    }

    /* By default, no rate limiting */
    if (num_rate == 0)
    {
        num_rate = 1;
        rate_value[0] = 0;
        rate_prob[0] = 100;
        rate_prob_total = rate_prob[0];
        if (verbose_mode)
            printf("Rate: %uMbps, Prob: %u\n", rate_value[0], rate_prob[0]);
    }

}

/* Set request variables */
void set_req_variables()
{
    int i = 0;
    unsigned long req_size_total = 0;
    unsigned long req_interval_total = 0;
    unsigned long rate_total = 0;
    double dscp_total = 0;

    /* calculate average request arrival interval */
    if (load > 0)
    {
        period_us = avg_CDF(req_size_dist) * 8 / load / TG_GOODPUT_RATIO;
        if (period_us <= 0)
        {
            cleanup();
            error("Error: period_us is not positive");
        }
    }
    else
    {
        cleanup();
        error("Error: load is not positive");
    }

    /* transfer time to the number of requests */
    if (req_total_num == 0 && req_total_time > 0)
        req_total_num = max((unsigned long)req_total_time * 1000000 / period_us, 1);

    /* request variables */
    req_size = (unsigned int*)calloc(req_total_num, sizeof(unsigned int));
    req_server_id = (unsigned int*)calloc(req_total_num, sizeof(unsigned int));
    req_dscp = (unsigned int*)calloc(req_total_num, sizeof(unsigned int));
    req_rate = (unsigned int*)calloc(req_total_num, sizeof(unsigned int));
    req_sleep_us = (unsigned int*)calloc(req_total_num, sizeof(unsigned int));
    req_start_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));
    req_stop_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));

    if (!req_size || !req_server_id || !req_dscp || !req_rate || !req_sleep_us || !req_start_time || !req_stop_time)
    {
        cleanup();
        error("Error: calloc per-request variables");
    }

    for (i = 0; i < req_total_num; i++)
    {
        req_size[i] = gen_random_CDF(req_size_dist);    //flow size
        req_server_id[i] = rand() % num_server; //server ID
        server_req_count[req_server_id[i]]++;   //per-server request number
        req_dscp[i] = gen_value_weight(dscp_value, dscp_prob, num_dscp, dscp_prob_total);    //flow DSCP
        req_rate[i] = gen_value_weight(rate_value, rate_prob, num_rate, rate_prob_total);   //flow sending rate
        req_sleep_us[i] = poission_gen_interval(1.0/period_us); //sleep interval based on poission process

        req_size_total += req_size[i];
        req_interval_total += req_sleep_us[i];
        dscp_total += req_dscp[i];
        rate_total += req_rate[i];
    }

    printf("===========================================\n");
    printf("We generate %u requests in total\n", req_total_num);

    for (i = 0; i < num_server; i++)
        printf("%s:%u    %u requests\n", server_addr[i], server_port[i], server_req_count[i]);

    printf("===========================================\n");
    printf("The average reuest qarrival interval is %lu us\n", req_interval_total/req_total_num);
    printf("The average request size is %lu bytes\n", req_size_total/req_total_num);
    printf("The average DSCP value is %.2f\n", dscp_total/req_total_num);
    printf("The average flow sending rate is %lu Mbps\n", rate_total/req_total_num);
    printf("The expected experiment duration is %lu s\n", req_interval_total/1000000);
}

/* Receive traffic from established connections */
void *listen_connection(void *ptr)
{
    struct Conn_Node* node = (struct Conn_Node*)ptr;
    unsigned int meta_data_size = 4 * sizeof(unsigned int);
    unsigned int flow_id, flow_size, flow_tos, flow_rate;   //flow request meta data
    char read_buf[TG_MAX_READ] = {'\0'};

    while (true)
    {
        if (read_exact(node->sockfd, read_buf, meta_data_size, meta_data_size, false) != meta_data_size)
        {
            perror("Error: read meata data");
            break;
        }

        /* extract meta data */
        memcpy(&flow_id, read_buf, sizeof(unsigned int));
        memcpy(&flow_size, read_buf + sizeof(unsigned int), sizeof(unsigned int));
        memcpy(&flow_tos, read_buf + 2 * sizeof(unsigned int), sizeof(unsigned int));
        memcpy(&flow_rate, read_buf + 3 * sizeof(unsigned int), sizeof(unsigned int));

        if (read_exact(node->sockfd, read_buf, flow_size, TG_MAX_READ, true) != flow_size)
        {
            perror("Error: receive flow");
            break;
        }

        node->busy = false;
        pthread_mutex_lock(&(node->list->lock));
        if (flow_id != 0) //Not the special flow ID
        {
            node->list->flow_finished++;
            node->list->available_len++;
        }
        /* Ohterwise, it's a special flow ID to terminate connection.
           So this connection will no longer be available. */
        pthread_mutex_unlock(&(node->list->lock));

        /* A special flow ID to terminate persistent connection */
        if (flow_id == 0)
            break;
        else
            gettimeofday(&req_stop_time[flow_id - 1], NULL);
    }

    close(node->sockfd);
    node->connected = false;
    node->busy = false;

    return (void*)0;
}

/* Generate flow requests */
void run_requests()
{
    unsigned int i = 0;
    unsigned int sleep_us = 0;

    for (i = 0; i < req_total_num; i++)
    {
        sleep_us += req_sleep_us[i];
        if (sleep_us > usleep_overhead_us)
        {
            usleep(sleep_us - usleep_overhead_us);
            sleep_us = 0;
        }
        run_request(i);
    }
}

/* Generate a flow request to the server */
void run_request(unsigned int req_id)
{
    unsigned int server_id = req_server_id[req_id];
    int sockfd;
    unsigned int meta_data_size = 4 * sizeof(unsigned int);
    char buf[4 * sizeof(unsigned int)] = {'\0'}; // buffer to hold meta data
    unsigned int flow_id = req_id + 1; //We reserve flow ID 0 for special usage
    unsigned int flow_size = req_size[req_id];
    unsigned int flow_tos = req_dscp[req_id] * 4;   //ToS = DSCP * 4
    unsigned int flow_rate = req_rate[req_id];
    struct Conn_Node* node = Search_Conn_List(&connection_lists[server_id]);
    unsigned int active_connections = 0;
    unsigned int i = 0;

    /* Cannot find available connection. Need to establish new connections. */
    if (!node)
    {
        if (Insert_Conn_List(&connection_lists[server_id], 1))
        {
            node = connection_lists[server_id].tail;
            if (verbose_mode)
                printf("[%u] Establish a new connection to %s:%u (available/total = %u/%u)\n", ++num_new_conn, server_addr[server_id], server_port[server_id], node->list->available_len, node->list->len);
            pthread_create(&(node->thread), NULL, listen_connection, (void*)node);  //start thread on this new connection
        }
        else
        {
            if (verbose_mode)
                printf("Cannot establish a new connection to %s:%u\n", server_addr[server_id], server_port[server_id]);
            return;
        }
    }

    if (verbose_mode && (req_id % 100 == 0))
    {
        active_connections = 0;
        for (i = 0; i< num_server; i++)
            active_connections += connection_lists[i].len - connection_lists[i].available_len;
        printf("Concurrent active connections: %u\n", active_connections);
    }

    /* fill in meta data */
    memcpy(buf, &flow_id, sizeof(unsigned int));
    memcpy(buf + sizeof(unsigned int), &flow_size, sizeof(unsigned int));
    memcpy(buf + 2 * sizeof(unsigned int), &flow_tos, sizeof(unsigned int));
    memcpy(buf + 3 * sizeof(unsigned int), &flow_rate, sizeof(unsigned int));

    /* Send request and record start time */
    gettimeofday(&req_start_time[req_id], NULL);
    sockfd = node->sockfd;
    node->busy = true;
    pthread_mutex_lock(&(node->list->lock));
    node->list->available_len--;
    pthread_mutex_unlock(&(node->list->lock));

    if(write_exact(sockfd, buf, meta_data_size, meta_data_size, 0, flow_tos, 0, false) != meta_data_size)
        perror("Error: write meta data");
}

/* Terminate all existing connections */
void exit_connections()
{
    unsigned int i = 0;
    struct Conn_Node* ptr = NULL;
    unsigned int num = 0;

    /* Start threads to receive traffic */
    for (i = 0; i < num_server; i++)
    {
        num = 0;
        ptr = connection_lists[i].head;
        while (true)
        {
            if (!ptr)
                break;
            else
            {
                if (ptr->connected)
                {
                    exit_connection(ptr);
                    num++;
                }
                ptr = ptr->next;
            }
        }
        Wait_Conn_List(&connection_lists[i]);
        if (verbose_mode)
            printf("Exit %u/%u connections to %s:%u\n", num, connection_lists[i].len, server_addr[i], server_port[i]);
    }
}

/* Terminate a connection */
void exit_connection(struct Conn_Node* node)
{
    int sockfd;
    unsigned int meta_data_size = 4 * sizeof(unsigned int);
    char buf[4 * sizeof(unsigned int)] = {'\0'}; // buffer to hold meta data
    unsigned int flow_id = 0; //A special flow ID to terminate connection
    unsigned int flow_size = 100;
    unsigned int flow_tos = 0;
    unsigned int flow_rate = 0;

    if (!node)
        return;

    memcpy(buf, &flow_id, sizeof(unsigned int));
    memcpy(buf + sizeof(unsigned int), &flow_size, sizeof(unsigned int));
    memcpy(buf + 2 * sizeof(unsigned int), &flow_tos, sizeof(unsigned int));
    memcpy(buf + 3 * sizeof(unsigned int), &flow_rate, sizeof(unsigned int));
    sockfd = node->sockfd;

    pthread_mutex_lock(&(node->list->lock));
    node->list->available_len--;
    pthread_mutex_unlock(&(node->list->lock));

    if(write_exact(sockfd, buf, meta_data_size, meta_data_size, 0, flow_tos, 0, false) != meta_data_size)
        perror("Error: write meta data");
}

void print_statistic()
{
    unsigned long long duration_us = (tv_end.tv_sec - tv_start.tv_sec) * 1000000 + tv_end.tv_usec - tv_start.tv_usec;
    unsigned long long req_size_total = 0;
    unsigned long long fct_us;
    unsigned int flow_goodput_mbps;    //per-flow goodput (Mbps)
    unsigned int goodput_mbps; //total goodput (Mbps)
    unsigned int i = 0;
    FILE *fd = NULL;

    fd = fopen(fct_log_name, "w");
    if (!fd)
        error("Error: fopen");

    for (i = 0; i < req_total_num; i++)
    {
        req_size_total += req_size[i];
        fct_us = (req_stop_time[i].tv_sec - req_start_time[i].tv_sec) * 1000000 + req_stop_time[i].tv_usec - req_start_time[i].tv_usec;
        if (fct_us > 0)
            flow_goodput_mbps = req_size[i] * 8 / fct_us;
        else
            flow_goodput_mbps = 0;

        fprintf(fd, "%u %llu %u %u %u\n", req_size[i], fct_us, req_dscp[i], req_rate[i], flow_goodput_mbps);    //size, FCT(us), DSCP, sending rate (Mbps), goodput (Mbps)

        if ((req_stop_time[i].tv_sec == 0) && (req_stop_time[i].tv_usec == 0))
            printf("Unfinished flow request %u\n", i);
    }

    fclose(fd);
    goodput_mbps = req_size_total * 8 / duration_us;
    printf("The actual RX throughput is %u Mbps\n", (unsigned int)(goodput_mbps/TG_GOODPUT_RATIO));
    printf("The actual duration is %llu s\n", duration_us/1000000);
    printf("===========================================\n");
    printf("Write FCT results to %s\n", fct_log_name);
}

/* Clean up resources */
void cleanup()
{
    unsigned int i = 0;

    free(server_port);
    free(server_addr);
    free(server_req_count);

    free(dscp_value);
    free(dscp_prob);

    free(rate_value);
    free(rate_prob);

    free_CDF(req_size_dist);
    free(req_size_dist);

    free(req_size);
    free(req_server_id);
    free(req_dscp);
    free(req_rate);
    free(req_sleep_us);
    free(req_start_time);
    free(req_stop_time);

    if (connection_lists)
    {
        if (verbose_mode)
            printf("===========================================\n");

        for(i = 0; i < num_server; i++)
        {
            if (verbose_mode)
                printf("Clear connection list %u to %s:%u\n", i, connection_lists[i].ip, connection_lists[i].port);
            Clear_Conn_List(&connection_lists[i]);
        }
    }
    free(connection_lists);
}
