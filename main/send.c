#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_example.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#define MAC_LEN 6
#define RREQ 1
#define RREP 2
#define DATA 3
#define HELLO 5
#define RERR 4
static uint8_t broadcast_add[MAC_LEN] = {0xFF,0XFF,0xFF,0xFF,0xFF,0xFF};
int table_update_count = 0;
esp_timer_handle_t timer1;
int hello_array[10] = {0};

SemaphoreHandle_t Mutex;

typedef struct message{
    int type; //封包類別(接收端根据類別來做不同處理)
    uint8_t source[6]; //封包傳送初始端
    uint8_t destination[6]; //封包的target
    int sequence_number; //此部分還不知道如何使用
    int hop_count; //封包已經經過多少節點
    char data[100]; //當type == DATA，這邊是要傳送的訊息內容
}message;

message send_data;
message send_RREQ;
message send_RREP;
message send_HELLO;
message send_RERR;

message recv_data;
message recv_RREQ;
message recv_RREP;
message recv_RERR;
message recv_HELLO;
typedef struct Route_Table{
    uint8_t destination[MAC_LEN];    //這個節點可以到哪個destination
    uint8_t next_hop[MAC_LEN];       //到達該destination的下一個節點
    int hop_count;                   //要經過幾個節點
    int sequence_number; //還不清楚如何應用
    bool valid;  //表示該route table 項是否有效(尚未使用到)
}Route_Table;

Route_Table table[10]; //可以存到達10個不同node的route table

void handle_RREQ();
void handle_RREP();
void handle_RERR();
void handle_HELLO();
void send_RREP_message();
void send_RREQ_message();
void send_RERR_message();
void send_Hello();
int check_route_table(uint8_t des_add[6]);

bool compare_self_MAC_ADD(uint8_t compare_ADD[6]){
    uint8_t self_MAC_ADD[6];
    if(esp_efuse_mac_get_default(self_MAC_ADD)== ESP_OK){
        int count = 0;
        for(int i = 0 ; i < MAC_LEN ; i++){
            if(compare_ADD[i] == self_MAC_ADD[i]){
                count++;
            }
        }
        if(count == 6){
            printf("%02X:%02X:%02X:%02X:%02X:%02X matched %02X:%02X:%02X:%02X:%02X:%02X\n",compare_ADD[0],compare_ADD[1],compare_ADD[2],compare_ADD[3],compare_ADD[4],compare_ADD[5], self_MAC_ADD[0], self_MAC_ADD[1], self_MAC_ADD[2], self_MAC_ADD[3], self_MAC_ADD[4], self_MAC_ADD[5]);
            return true;
        }
    }
    return false;
}

void show_route_table(){
    int i;
    printf("\tdest \t\t nexthop \t\t hop count \t valid\n");
    for(i = 0 ; i < table_update_count ; i++){
        printf("%02X:%02X:%02X:%02X:%02X:%02X \t %02X:%02X:%02X:%02X:%02X:%02X \t %d  \t\t",table[i].destination[0],table[i].destination[1],table[i].destination[2],table[i].destination[3],table[i].destination[4],table[i].destination[5],table[i].next_hop[0],table[i].next_hop[1],table[i].next_hop[2],table[i].next_hop[3],table[i].next_hop[4],table[i].next_hop[5],table[i].hop_count);
        if(table[i].valid == true){
            printf("true");
        }
        else{
            printf("false");
        }
        printf("\n");
    }
}

void route_table_init(){    //在此先清空所創建的route table
    for (int i = 0 ; i < 10 ; i++ ){  
        for(int j = 0 ; j < MAC_LEN ; j++){
            table[i].destination[j] = 0xFF;
            table[i].next_hop[j] = 0xFF;
        }
        table[i].hop_count = 0;
        table[i].sequence_number = 0;
        table[i].valid = false;
    }
    uint8_t self_MAC_ADD[6];
    if(esp_efuse_mac_get_default(self_MAC_ADD)== ESP_OK){  //將自己的地址作為第一個route table 資料
        memcpy(table[0].destination, self_MAC_ADD, sizeof(self_MAC_ADD));
        memcpy(table[0].next_hop, self_MAC_ADD, sizeof(self_MAC_ADD));
        table[0].hop_count = 0;
        table[0].sequence_number = 0;
        table[0].valid = true;
    }
    table_update_count += 1;
}

static void wifi_init(void) //init the wifi connection
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void  send_callback(const uint8_t *mac_addr, esp_now_send_status_t status) { //2 因為將下一次的傳送寫到send_callback中，每次執行send都會跳進send_callback，因此可以重復send的動作
    if(status == ESP_NOW_SEND_SUCCESS){
        printf("send success\n");
    }
    else{
        printf("send fail\n");
    }
    printf("*****send_callback end******\n");
    //vTaskDelay(1000 / portTICK_PERIOD_MS);
}

void recv_callback(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len) { //3
    //esp_now_info 中有存sender 的 MAC_ADDRESS

    memcpy(&recv_data,data,data_len); //將收到的data從 copy 到 recv_data中
    uint8_t sender[MAC_LEN];
    memcpy(sender,esp_now_info->src_addr, sizeof(sender)); //將傳送者的MAC存起來為了更新next_hop，或是為了RREP訊息tracaback

    printf("\n*********\n%d byte from %02X:%02X:%02X:%02X:%02X:%02X\n",data_len, esp_now_info->src_addr[0],esp_now_info->src_addr[1],esp_now_info->src_addr[2],esp_now_info->src_addr[3],esp_now_info->src_addr[4],esp_now_info->src_addr[5]);
    printf("type = %d\n",recv_data.type);

    bool compare;
    compare = compare_self_MAC_ADD(recv_data.source); //false 代表收到的訊息並不是剛剛從自己傳出右被廣播回來的

    while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);

    if(recv_data.type == RREQ && compare == false){ //再依據分類，RREQ都是broadcast，因此可能收到自己傳出的message被廣播回來
        memcpy(&recv_RREQ, &recv_data, sizeof(recv_data));
        printf("receive and handel_RREQ\n");
        handle_RREQ(sender); //上一個node
    }
    if(recv_data.type == RREP){ //RREP 是按固定的路線反向傳，因此可能沒有重複收到自己的訊息的問題
        memcpy(&recv_RREP, &recv_data, sizeof(recv_data));
        printf("receive and handle_RREP\n");
        handle_RREP(sender); //sender 為了更新 route table
    }
    if(recv_data.type == RERR){
        
        memcpy(&recv_RERR, &recv_data, sizeof(recv_data));
        handle_RERR();
    }
    if(recv_data.type == HELLO){
        
        memcpy(&recv_HELLO, &recv_data, sizeof(recv_data));
        handle_HELLO();
    }

    xSemaphoreGive(Mutex);

    printf("*****recv_callback end******\n");
    //vTaskDelay(1000 / portTICK_PERIOD_MS);

    memset(&recv_data,0,sizeof(recv_data));
    memset(&send_RREP,0,sizeof(recv_RREP));
    memset(&send_RREQ,0,sizeof(recv_RREQ));
}

void send_RREQ_message(){
    //注意當再次傳送RREQ訊息，hop_count要+1
    //將接受到的recv_RREQ再次broadcast出去，hop_count要+1
    memcpy(&send_RREQ, &recv_RREQ, sizeof(recv_RREQ));
    send_RREQ.hop_count += 1;
    esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_RREQ, sizeof(send_RREQ));
    if(result == ESP_OK){
        printf("no matched, broadcasted RREQ again\n");
    }
}

void handle_RREQ(uint8_t RREQ_sender[MAC_LEN]){
    //check the route table
    printf("handling RREQ\n");
    printf("source [%02X:%02X:%02X:%02X:%02X:%02X] want get to destination [%02X:%02X:%02X:%02X:%02X:%02X]\n",recv_RREQ.source[0],recv_RREQ.source[1],recv_RREQ.source[2],recv_RREQ.source[3],recv_RREQ.source[4],recv_RREQ.source[5], recv_RREQ.destination[0],recv_RREQ.destination[1],recv_RREQ.destination[2],recv_RREQ.destination[3],recv_RREQ.destination[4],recv_RREQ.destination[5]);
    int check = check_route_table(recv_RREQ.destination); //route table 有目標節點的資訊
    if(check != 100){ //表示在route table 找到對應的route資訊
        printf("found matched, send RREP back to [%02X:%02X:%02X:%02X:%02X:%02X]\n", RREQ_sender[0], RREQ_sender[1], RREQ_sender[2], RREQ_sender[3], RREQ_sender[4], RREQ_sender[5]);
        send_RREP_message(RREQ_sender, (check%200) + 200); //This node first has destination route, send RREP message back to the previous RREQ sender (1)
    }
    else{
        send_RREQ_message();//send the RREQ if there isn't identical in route table
    }
    //update the route table (most importtant part)，反向更新(將RREQ的source當成table.destination，RREQ 的sender 當作 next_hop，RREQ 的 hopcount 當作table.hop_count
    //若已經有到達RREQ.source資訊就比較hopcount
    check = check_route_table(recv_RREQ.source);
    if((check == 100) ||(check != 100 && table[check].hop_count > recv_RREQ.hop_count && check < 200)){ //not found or valid+smaller hop_count
        memcpy(table[table_update_count].destination, recv_RREQ.source, sizeof(recv_RREQ.source));  
        memcpy(table[table_update_count].next_hop, RREQ_sender, sizeof(table[table_update_count].next_hop)); //到達source 的 next_hop 是 RREQ_sender
        table[table_update_count].hop_count = recv_RREQ.hop_count;
        table[table_update_count].valid = true;
        table_update_count++; 
    }
    else if(check > 200 ){ //存過但是valid == false
        memcpy(table[table_update_count].destination, recv_RREQ.source, sizeof(recv_RREQ.source));  
        memcpy(table[table_update_count].next_hop, RREQ_sender, sizeof(table[table_update_count].next_hop)); //到達source 的 next_hop 是 RREQ_sender
        table[table_update_count].hop_count = recv_RREQ.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    show_route_table();
}

void send_RREP_message(uint8_t trace_back[6], int RREP_type){
    //得傳回對應的資訊列(hopcount)
    uint8_t RREP_send_add[MAC_LEN];
    if(RREP_type != 2){ //是第一則RREP的傳送 =>> route table 有找到到達的路徑
        send_RREP.type = RREP;
        memcpy(send_RREP.destination, recv_RREQ.source, sizeof(recv_RREQ.source));
        memcpy(send_RREP.source, recv_RREQ.destination, sizeof(recv_RREQ.destination)); //此處改過，先前是 recv_RREQ.destination
        send_RREP.hop_count = table[RREP_type - 200].hop_count + 1; //有問題處 //有可能是中間節點回傳
        memcpy(RREP_send_add, trace_back, sizeof(RREP_send_add));
    }
    if(RREP_type == 2){//是轉發的RREP
    int check = check_route_table(recv_RREP.destination);
        if(check != 100){ //表示在route table 找到對應的route資訊
            printf("found matched, send RREP back to [%02X:%02X:%02X:%02X:%02X:%02X] though [%02X:%02X:%02X:%02X:%02X:%02X], hop count = %d\n", table[check].destination[0], table[check].destination[1], table[check].destination[2], table[check].destination[3], table[check].destination[4], table[check].destination[5], table[check].next_hop[0], table[check].next_hop[1], table[check].next_hop[2], table[check].next_hop[3], table[check].next_hop[4], table[check].next_hop[5], table[check].hop_count);
            send_RREP.type = RREP;
            memcpy(RREP_send_add, table[check].next_hop, sizeof(RREP_send_add));
            memcpy(send_RREP.destination, recv_RREP.destination, sizeof(recv_RREP.destination));
            memcpy(send_RREP.source, recv_RREP.source, sizeof(recv_RREP.source));
            send_RREP.hop_count = recv_RREP.hop_count + 1;
        }
    }

    printf("RREP to %02X:%02X:%02X:%02X:%02X:%02X\n",RREP_send_add[0],RREP_send_add[1],RREP_send_add[2],RREP_send_add[3],RREP_send_add[4],RREP_send_add[5]);

    bool check_peer = esp_now_is_peer_exist(RREP_send_add);
    if(check_peer == false){
        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
        memset(peer, 0, sizeof(esp_now_peer_info_t));
        peer->channel = CONFIG_ESPNOW_CHANNEL;
        peer->ifidx = ESPNOW_WIFI_IF;
        peer->encrypt = false;
        memcpy(peer->peer_addr, RREP_send_add, MAC_LEN);
        ESP_ERROR_CHECK( esp_now_add_peer(peer) ); 
    }

    //send_back.valid = true;

    esp_err_t result = esp_now_send(RREP_send_add, (uint8_t *)&send_RREP, sizeof(send_RREP));
    if(result == ESP_OK){
        printf("sending RREP\n");
    }
}

void handle_RREP(uint8_t trace_back_node[MAC_LEN]){
    // check if send back to original node
    printf("handling RREP\n");
    uint8_t self_MAC_ADD[6] = {0XFF,0XFF,0XFF,0XFF,0XFF,0XFF};
    
    /*update route table*/
    //check if the route table have already exist this route data
    //if it does, compare this route data hop_count with the one of table
    //small one can stay
    int check = check_route_table(recv_RREP.source);
    printf("[check = %d, recv_RREP.hop_count = %d]\n",check,recv_RREP.hop_count);
    if((check == 100) || ((check != 100) && (table[check].hop_count > recv_RREP.hop_count) && (check < 200))){//沒找到 or 找到valid且table.hopcount較大 =>>>> 新增
        memcpy(table[table_update_count].destination,recv_RREP.source, MAC_LEN);
        memcpy(table[table_update_count].next_hop, trace_back_node, MAC_LEN);
        table[table_update_count].hop_count = recv_RREP.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    else if(check > 200){ //存過但是valid == false =>>>> 換成true 
        memcpy(table[table_update_count].destination,recv_RREP.source, MAC_LEN);
        memcpy(table[table_update_count].next_hop, trace_back_node, MAC_LEN);
        table[table_update_count].hop_count = recv_RREP.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    show_route_table();
    
    //send rrep to original point
    if(esp_efuse_mac_get_default(self_MAC_ADD) == ESP_OK){
        int MAC_ADD_compare = 0;
        for(int i = 0; i < MAC_LEN ; i++){
            if(self_MAC_ADD[i] == recv_RREP.destination[i]){
                MAC_ADD_compare++;
            }
        }
        if(MAC_ADD_compare != 6){ //還沒傳回original node 就要再轉發(2)
            send_RREP_message(trace_back_node,2);
        }
        if(MAC_ADD_compare == 6){
            printf("RREP sent back to source node\n");
        }
    }
}

void handle_HELLO(){
    //將hello訊息的 source 與 route table 中的 next_hop 比較
    int count = 0, count_total = 0;
    for(int i = 1 ; i < table_update_count ; i++){ //i 是 table 第i列
        count = 0;
        for(int j = 0 ; j < MAC_LEN ; j++){
            //printf("[%02X vs %02X]",table[i].next_hop[j],recv_HELLO.source[j]);
            if(table[i].next_hop[j] == recv_HELLO.source[j]){
                count++;
            }
        }
        if(count == 6){ /*找到對應的next_hop，路徑仍有效*/
            hello_array[i] = 1; //記住對應的路徑
            printf("[found exist %d]",i);
            count_total++;
        }
    }
    if(count_total == 0){ //HELLO packet的source沒有對應到route table中的任何neighbor node => 代表這是新的節點
        printf("[count = %d]\n",count);
        memcpy(table[table_update_count].destination, recv_HELLO.source, MAC_LEN);
        memcpy(table[table_update_count].next_hop, recv_HELLO.source, MAC_LEN);
        table[table_update_count].valid = true;
        table[table_update_count].hop_count = 1;
        table_update_count++; 
        show_route_table();
    }
}

void send_Hello(){
    //init the HELLO message
    printf("sending HELLO\n");
    send_HELLO.type = HELLO;
    send_HELLO.hop_count = 1;
    //printf("[table_count = %d]\n",table_update_count);
    //send Hello message to every nexthop in route table
    uint8_t self_MAC_ADD[6];
    if(esp_efuse_mac_get_default(self_MAC_ADD) ==ESP_OK){
            memcpy(send_HELLO.source, self_MAC_ADD, sizeof(self_MAC_ADD));
        }
    esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_HELLO, sizeof(send_HELLO));
    if(result == ESP_OK){
        printf("broadcasted HELLO");
    }
    /*for(int i = 1 ; i < table_update_count ; i++){ //table[0]是自己
        bool check_peer = esp_now_is_peer_exist(table[i].next_hop);
        if(check_peer == false){
            esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
            memset(peer, 0, sizeof(esp_now_peer_info_t));
            peer->channel = CONFIG_ESPNOW_CHANNEL;
            peer->ifidx = ESPNOW_WIFI_IF;
            peer->encrypt = false;
            memcpy(peer->peer_addr, table[i].next_hop, MAC_LEN);
            ESP_ERROR_CHECK( esp_now_add_peer(peer) ); 
        }
        uint8_t self_MAC_ADD[6];
        if(esp_efuse_mac_get_default(self_MAC_ADD) ==ESP_OK){
            memcpy(send_HELLO.source, self_MAC_ADD, sizeof(self_MAC_ADD));
            memcpy(send_HELLO.destination, table[i].next_hop, sizeof(self_MAC_ADD));
        }
        
        //send_back.valid = true;
        esp_err_t result = esp_now_send(table[i].next_hop, (uint8_t *)&send_HELLO, sizeof(send_HELLO));
        if(result == ESP_OK){
            printf("sending HELLO to [%02X:%02X:%02X:%02X:%02X:%02X]\n",table[i].next_hop[0],table[i].next_hop[1],table[i].next_hop[2],table[i].next_hop[3],table[i].next_hop[4],table[i].next_hop[5]);
        }
    }*/
}

void send_RERR_message(){
    // broadcast the information about the error node 
    for(int i = 0; i < table_update_count ; i++){
        if(table[i].valid == false){
            send_RERR.type = RERR;
            memcpy(send_RERR.destination ,table[i].next_hop, MAC_LEN);
            bool check_peer = esp_now_is_peer_exist(broadcast_add);
            if(check_peer == false){
                esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                memset(peer, 0, sizeof(esp_now_peer_info_t));
                peer->channel = CONFIG_ESPNOW_CHANNEL;
                peer->ifidx = ESPNOW_WIFI_IF;
                peer->encrypt = false;
                memcpy(peer->peer_addr, broadcast_add, MAC_LEN);
                ESP_ERROR_CHECK( esp_now_add_peer(peer) ); 
                esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_RERR, sizeof(send_RERR));
                if(result == ESP_OK){
                    printf("broadcasted RERR\n");
                }
            } 
        }
    }
}

void handle_RERR(){
    printf("handling RERR\n");
    for(int i = 0 ; i < table_update_count ; i++){//將收到的RERR訊息中的destination拿出比對
        int count1 = 0, count2 = 0;
        for(int j = 0; j < MAC_LEN ; j++){
            if(table[i].next_hop[j] == recv_RERR.destination[j]){
                count1++;
            }
            if(table[i].destination[j] == recv_RERR.destination[j]){
                count2++;
            }
        }
        if(count1 == 6 || count2 == 6){//符合route table中的next_hop 或 destination
            table[i].valid = false;//標記為invalid
        }
    }
}

void vTask1(void *p){ //定期廣播HELLO packet
    while(1){
        printf("taks1 running\n");
        while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
        send_Hello();
        xSemaphoreGive(Mutex);
        vTaskDelay(15000/ portTICK_PERIOD_MS);
    }
}

void Task2(void *p){ //調整 route table
    while(1){
        int mark_error_change = 0; //等於 0 代表route table中沒有從 true => false
        printf("Task2 running\n");
        while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
        for(int i = 1; i < table_update_count ; i++){ //table[0]是自己=>從table[1]開始
            if(hello_array[i]){
                table[i].valid = true;
            }
            else{
                if(table[i].valid == true){
                    mark_error_change = 1;
                }
                table[i].valid = false;    //invalid沒收到的資訊
            }
        }
        xSemaphoreGive(Mutex);
        show_route_table();
        
        //broadcast 對應的invalid路徑資訊( type=RERR,填寫des資料以表示到達des的路徑已經斷)
        if(mark_error_change){
            send_RERR_message();
        }
        memset(hello_array, 0, sizeof(hello_array));
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}
int check_route_table(uint8_t des_add[6]){
    int count = 0;
    int valid_flag = -1;
    int invalid_flag = -1;
    //uint8_t self_MAC_ADD[6];
    for(int i = 0 ; i < 10 ; i++){
        count = 0;
        for(int j = 0 ; j < MAC_LEN ; j++){  //find one line in the array(have 10 line total)
            if(table[i].destination[j] == des_add[j])
                count++;
        }
        if(count == 6 && table[i].valid == true){
        //return 該destination的資訊(destination，next_hop，hop_count ,...)or 該路徑在table的位置?
            //return i;
            //break;
            valid_flag = i;
        }
        else if(count == 6 && table[i].valid == false){
            //return 200+i; //標記存過路徑但已經毀壞(第i個路徑)
            invalid_flag = 200+i;
        }
    }
    if(0 <= valid_flag && valid_flag < 10){ 
        return valid_flag;
    }
    else if(invalid_flag >= 200){
        return invalid_flag;
    }
    return 100; //沒有找到
}
void app_main(void){

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    wifi_init();
    esp_now_init();

    esp_now_register_send_cb(send_callback);
    esp_now_register_recv_cb(recv_callback);

    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        printf("Malloc peer information fail");
        esp_now_deinit();
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    //複製s_ex_br_mac  到 peer_addr
    memcpy(peer->peer_addr, broadcast_add, MAC_LEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );

    /*configuring uart*/
    const uart_port_t uart_num = UART_NUM_2;
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
    };
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, 17, 16, -1, -1));
    uart_driver_install(uart_num, 512, 1024, 0, NULL, 0);
    char gps_data[512];
    int length = 0;
    char GPGGA[6] = "GPGGA";
    ESP_ERROR_CHECK(uart_get_buffered_data_len(uart_num, (size_t *)&length));


    Mutex = xSemaphoreCreateCounting(1,1); //為了控制進入更改route table 

    //為了能與收到的rreq_message中的destination_add比較，先將自己的mac_address讀取出來******************
    uint8_t self_MAC_ADD[6] = {0XFF,0XFF,0XFF,0XFF,0XFF,0XFF};
    if(esp_efuse_mac_get_default(self_MAC_ADD) == ESP_OK){
        printf("MAC_ADDRESS of this board is: [%02X:%02X:%02X:%02X:%02X:%02X]\n", self_MAC_ADD[0],self_MAC_ADD[1],self_MAC_ADD[2],self_MAC_ADD[3],self_MAC_ADD[4],self_MAC_ADD[5]);
    }

    printf("start send side**********\n"); 

    route_table_init();

    uint8_t target[MAC_LEN] = {0X30, 0XC9, 0X22, 0X12, 0XEC, 0XC4};
    xTaskCreate(&vTask1,"task1", 2048, NULL, 1, NULL);
    xTaskCreate(&Task2, "task2", 2048, NULL, 1, NULL);
    while(1){
        while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
        int check = check_route_table(target);
        if(check != 100 && check < 200){ //if this return !=100 < 200value, we have the destination information in the route table
            //照route table 的information 去執行對應的傳送方式，check 是對應的route table 的line
            bool check_peer = esp_now_is_peer_exist(table[check].next_hop);
            if(check_peer == false){
                esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                memset(peer, 0, sizeof(esp_now_peer_info_t));
                peer->channel = CONFIG_ESPNOW_CHANNEL;
                peer->ifidx = ESPNOW_WIFI_IF;
                peer->encrypt = false;
                memcpy(peer->peer_addr, table[check].next_hop, MAC_LEN);
                ESP_ERROR_CHECK( esp_now_add_peer(peer) ); 
            }
            //init the data
            uart_flush(UART_NUM_2);
            length = uart_read_bytes(uart_num, gps_data, 512, 80);
            if (length > 0)  //取得GPS資料
            {
                char word[100] = {'\0'};
                uint32_t *result_1 = strstr(gps_data, GPGGA); //提取 GPGGA 資料
                if(result_1 != NULL){
                    char *newline = strchr(result_1, '\n');
                    size_t length_to_next_line = (int)newline - (int)result_1; //直到換行鍵前的char數量   
                    strncpy(word, (const char *)result_1, length_to_next_line); //將GPGGA資料複製到傳送區
                }
                memcpy(send_data.data, word, sizeof(word));
                send_data.type = DATA; //封包類別(接收端跟)
                memcpy(send_data.source, self_MAC_ADD, sizeof(self_MAC_ADD));
                memcpy(send_data.destination, target, sizeof(target));
                send_data.hop_count = 1;
                esp_err_t result = esp_now_send(table[check].next_hop, (uint8_t *)&send_data,sizeof(send_data)); //send 動作
                ESP_ERROR_CHECK(result);
                if(result == ESP_OK){
                    printf("\nsent data from table\n");
                }
            }    
        }
        else{ //如果route table 沒有資訊，透過broadcast 來對附近的esp32做RREQ_message
            send_RREQ.type = RREQ;
            memcpy(send_RREQ.source, self_MAC_ADD, sizeof(self_MAC_ADD)); //source = self
            memcpy(send_RREQ.destination, target, sizeof(target)); //destination = target
            send_RREQ.hop_count = 1;
            send_RREQ.sequence_number = -1;
            esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_RREQ,sizeof(send_RREQ)); //send 動作
            ESP_ERROR_CHECK(result);
            if(result == ESP_OK){
                printf("\nbroadcast success RREQ message to [%02X:%02X:%02X:%02X:%02X:%02X]\n",send_RREQ.destination[0],send_RREQ.destination[1],send_RREQ.destination[2],send_RREQ.destination[3],send_RREQ.destination[4],send_RREQ.destination[5]);
            }
            else{
                printf("sent fail\n");
            }
        }
        xSemaphoreGive(Mutex);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}