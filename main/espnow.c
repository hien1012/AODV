#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
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

#define MAC_LEN 6
#define RREQ 1
#define RREP 2
#define DATA 3
#define RERR 4
#define HELLO 5
static uint8_t broadcast_add[MAC_LEN] = {0xFF,0XFF,0xFF,0xFF,0xFF,0xFF};
int table_update_count = 0;
int hello_array[10] = {0};
esp_timer_handle_t timer1;

SemaphoreHandle_t Mutex;

typedef struct message{
    int type; 
    uint8_t source[6];
    uint8_t destination[6];
    int sequence_number;
    int hop_count;
    char data_word[100]; 
}message;

message send_data;
message send_RREQ;
message send_RREP;
message send_HELLO;
message send_RERR;

message recv_data;
message recv_RREQ;
message recv_RREP;
message recv_HELLO;
message recv_RERR;

typedef struct Route_Table{
    uint8_t destination[MAC_LEN];    //這個節點可以到哪個destination
    uint8_t next_hop[MAC_LEN];         //到達該destination的下一個節點
    int hop_count;
    int sequence_number;
    bool valid; //表示該route table 項是否有效
}Route_Table;
Route_Table table[10]; //可以存到達10個不同路徑的route table

void handle_DATA();
void handle_RREQ();
void handle_RREP();
void handle_RERR();
void handle_HELLO();
void send_RREP_message();
void send_RREQ_message();
void send_RERR_message();
void send_Hello();// periodicly send a "HELLO" packet to the next_hop in the route table?
int check_route_table(uint8_t des_add[6]);

bool compare_self_MAC_ADD(uint8_t compare_ADD[6]){ //比較 compare_ADD 與 板子本身的MAC ADD 是否相同
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

void show_route_table(){  //print 出route table
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
    vTaskDelay(1000 / portTICK_PERIOD_MS);

}

void recv_callback(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len) { //3

    memcpy(&recv_data,data,data_len); //將收到的data從 copy 到 recv_data中
    uint8_t sender[MAC_LEN];
    memcpy(sender,esp_now_info->src_addr, sizeof(sender)); //將此packet傳送者的MAC存起來為了更新next_hop，或是為了RREP訊息traceback

    printf("\n==============\n%d byte from %02X:%02X:%02X:%02X:%02X:%02X\n",data_len, esp_now_info->src_addr[0],esp_now_info->src_addr[1],esp_now_info->src_addr[2],esp_now_info->src_addr[3],esp_now_info->src_addr[4],esp_now_info->src_addr[5]);
    printf("type = %d\n",recv_data.type);

    bool compare;
    compare = compare_self_MAC_ADD(recv_data.source); //false 代表收到的訊息並不是剛剛從自己傳出又被廣播回來的

    while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
    if(recv_data.type == RREQ && compare == false){ //再依據分類，RREQ都是broadcast，因此可能收到自己傳出的message被廣播回來
        memcpy(&recv_RREQ, &recv_data, sizeof(recv_data));
        printf("receive and handel_RREQ\n");
        handle_RREQ(sender);
    }
    if(recv_data.type == RREP){ //RREP 是按固定的路線反向傳，因此可能沒有重複收到自己的訊息的問題
        memcpy(&recv_RREP, &recv_data, sizeof(recv_data));
        printf("receive and handle_RREP\n");
        handle_RREP(sender); //傳sender是為了能藉由RREP packet更新 route table
    }
    if(recv_data.type == DATA){
        handle_DATA();
    }
    if(recv_data.type == RERR){
        memcpy(&recv_RERR, &recv_data, sizeof(recv_data));
        printf("receive and handle_RERR\n");
        handle_RERR();
        
    }
    if(recv_data.type == HELLO){
        memcpy(&recv_HELLO, &recv_data, sizeof(recv_data));
        printf("receive and handle_Hello\n");
        handle_HELLO();
    }
    xSemaphoreGive(Mutex);
    printf("*****recv_callback end******\n");
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    memset(&recv_data,0,sizeof(recv_data));
    memset(&send_RREP,0,sizeof(recv_RREP));
    memset(&send_RREQ,0,sizeof(recv_RREQ));
}

void handle_DATA(){
    bool compare;
    compare = compare_self_MAC_ADD(recv_data.destination);
    if(compare == true){ //自己是目標node
        printf("data = [%s]\n",recv_data.data_word);
    }
    if(compare == false){ //自己不是目標node，要查表來繼續轉播DATA packet
        //找route路徑，轉發到對應的next_hop
        int check;
        check = check_route_table(recv_data.destination);
        if(check != 100){
            printf("found route path\n");
            bool check_peer = esp_now_is_peer_exist(table[check].next_hop);
            if(check_peer == false){  //check peer
                esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                memset(peer, 0, sizeof(esp_now_peer_info_t));
                peer->channel = CONFIG_ESPNOW_CHANNEL;
                peer->ifidx = ESPNOW_WIFI_IF;
                peer->encrypt = false;
                memcpy(peer->peer_addr, table[check].next_hop, MAC_LEN);
                ESP_ERROR_CHECK( esp_now_add_peer(peer) ); 
            }
            recv_data.hop_count += 1;  //hop_count 往上加
            esp_err_t result = esp_now_send(table[check].next_hop, (uint8_t *)&recv_data, sizeof(recv_data));
            if(result == ESP_OK){
                printf("send data packet to next_hop [%02X:%02X:%02X:%02X:%02X:%02X]",table[check].next_hop[0],table[check].next_hop[1],table[check].next_hop[2],table[check].next_hop[3],table[check].next_hop[4],table[check].next_hop[5]);
            }
        }
    }
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
    int check = check_route_table(recv_data.destination); //route table 有目標節點的資訊
    printf("[check = %d]\n",check);
    if(check != 100){ //表示在route table 找到對應的route資訊
        printf("found matched, send RREP back to [%02X:%02X:%02X:%02X:%02X:%02X]\n", RREQ_sender[0], RREQ_sender[1], RREQ_sender[2], RREQ_sender[3], RREQ_sender[4], RREQ_sender[5]);
        send_RREP_message(RREQ_sender, (check % 200)+200); //This node is the destination, send RREP message back to the previous RREQ sender (1)
    }
    else{
        send_RREQ_message();//send the RREQ if there isn't identical in route table
    }
    //update the route table (most importtant part)，反向更新(將RREQ的source當成table.destination，RREQ 的sender 當作 next_hop，RREQ 的 hopcount 當作table.hop_count
    //若已經有到達RREQ.source資訊就比較hopcount
    check = check_route_table(recv_RREQ.source);
    printf("[check = %d, recv_RREP.hop_count = %d]\n",check,recv_RREQ.hop_count);
    if((check == 100) ||(check != 100 && table[check].hop_count > recv_RREQ.hop_count && check <200)){ // 目前沒有這路徑 or 有了但新的距離比較短
        memcpy(table[table_update_count].destination, recv_RREQ.source, sizeof(recv_RREQ.source)); //到達source 的 next_hop 是 RREQ_sender 
        memcpy(table[table_update_count].next_hop, RREQ_sender, sizeof(table[table_update_count].next_hop)); 
        table[table_update_count].hop_count = recv_RREQ.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    else if(check > 200 ){ //存過但是valid == false
        memcpy(table[table_update_count].destination, recv_RREQ.source, sizeof(recv_RREQ.source)); //到達source 的 next_hop 是 RREQ_sender 
        memcpy(table[table_update_count].next_hop, RREQ_sender, sizeof(table[table_update_count].next_hop)); 
        table[table_update_count].hop_count = recv_RREQ.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    show_route_table();
}

void send_RREP_message(uint8_t trace_back[6], int RREP_type){
    
    uint8_t RREP_send_add[MAC_LEN];
    if(RREP_type != 2){ //是第一則RREP的傳送
        send_RREP.type = RREP;
        memcpy(send_RREP.destination, recv_RREQ.source, sizeof(recv_RREQ.source));
        memcpy(send_RREP.source, recv_RREQ.destination, sizeof(recv_RREQ.destination));
        send_RREP.hop_count = table[RREP_type - 200].hop_count + 1;
        memcpy(RREP_send_add, trace_back, sizeof(RREP_send_add));
    }
    if(RREP_type == 2){ //rrep 轉發
    int check = check_route_table(recv_RREP.destination);
        if(check != 100){ //表示在route table 找到對應的route資訊
            printf("found matched, send RREP back to [%02X:%02X:%02X:%02X:%02X:%02X] through [%02X:%02X:%02X:%02X:%02X:%02X], hop count = %d\n", table[check].destination[0], table[check].destination[1], table[check].destination[2], table[check].destination[3], table[check].destination[4], table[check].destination[5], table[check].next_hop[0], table[check].next_hop[1], table[check].next_hop[2], table[check].next_hop[3], table[check].next_hop[4], table[check].next_hop[5], table[check].hop_count);
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

    printf("handling RREP\n");
    uint8_t self_MAC_ADD[6] = {0XFF,0XFF,0XFF,0XFF,0XFF,0XFF};
    
    //update route table
    //check if the route table have already exist this route data
    //if it does, compare "recv_RREP.hop_count" with the one of table (table[check].hop_count)
    //smaller one can stay
    int check = check_route_table(recv_RREP.source);// update route table
    printf("[check = %d, recv_RREP.hop_count = %d]\n",check,recv_RREP.hop_count);
    if((check == 100) || ((check != 100) && (table[check].hop_count > recv_RREP.hop_count) && (check < 200))){
        memcpy(table[table_update_count].destination,recv_RREP.source, MAC_LEN);
        memcpy(table[table_update_count].next_hop, trace_back_node, MAC_LEN);
        table[table_update_count].hop_count = recv_RREP.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    else if(check > 200 ){ //存過但是valid == false
        memcpy(table[table_update_count].destination,recv_RREP.source, MAC_LEN);
        memcpy(table[table_update_count].next_hop, trace_back_node, MAC_LEN);
        table[table_update_count].hop_count = recv_RREP.hop_count;
        table[table_update_count].valid = true;
        table_update_count++;
    }
    show_route_table();

    // check if send back to original node
    if(esp_efuse_mac_get_default(self_MAC_ADD) == ESP_OK){
        int MAC_ADD_compare = 0;
        for(int i = 0; i < MAC_LEN ; i++){
            if(self_MAC_ADD[i] == recv_RREP.destination[i]){
                MAC_ADD_compare++;
            }
        }
        if(MAC_ADD_compare != 6){ //還沒傳回original node 就要再轉發(標記為2)
            send_RREP_message(trace_back_node,2);
        }
        if(MAC_ADD_compare == 6){
            printf("RREP sent back to source node\n");
        }
    }
}

void handle_HELLO(){ //受到HELLO就將其加入route table
    printf("handling HELLO message\n[table count = %d]\n",table_update_count);
    //將hello訊息的 source 與 route table 中的 next_hop 比較
    int count = 0, count_total = 0;
    for(int i = 1 ; i < table_update_count ; i++){
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
    if(count_total == 0){ //HELLO packet 沒有對應到route table 中的任何neighbor node => 新加入的node 
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
    printf("start send HELLO\n");
    send_HELLO.type = HELLO;
    send_HELLO.hop_count = 1;
    uint8_t self_MAC_ADD[6];
    if(esp_efuse_mac_get_default(self_MAC_ADD) ==ESP_OK){
        memcpy(send_HELLO.source, self_MAC_ADD, sizeof(self_MAC_ADD)); //hello message格式 : 從self_MAC_ADD傳送的HELLO
    }
    esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_HELLO, sizeof(send_HELLO));
    if(result == ESP_OK){
        printf("broadcasted HELLO\n");
    }
}

void send_RERR_message(){
    // broadcast the information about the error node 
    printf("[table_update = %d]\n",table_update_count);
    for(int i = 1; i < table_update_count ; i++){
        if(table[i].valid == false){
            printf("[rerr found]\n");
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
            }
            esp_err_t result = esp_now_send(broadcast_add, (uint8_t *)&send_RERR, sizeof(send_RERR));
            if(result == ESP_OK){
                printf("broadcasted RERR\n");
            }
        }
    }
}

void handle_RERR(){
    for(int i = 1 ; i < table_update_count ; i++){//將收到的RERR訊息中的destination拿出比對
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

void Task1(void *p){
    while(1){
        printf("===================\ntaks1 running\n");
        while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
        send_Hello();
        xSemaphoreGive(Mutex);
        vTaskDelay(15000/ portTICK_PERIOD_MS);
    }
}

void Task2(void *p){
    while(1){
        int mark_error_change = 0; //等於 0 代表route table中沒有從 true => false
        printf("===================\nTask2 running\n");
        while(xSemaphoreTake(Mutex, portMAX_DELAY) != pdTRUE);
        for(int j = 0; j<10; j++){
            printf("%d",hello_array[j]);
        }
        printf("\n");
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
            printf("[there was changed]\n");
        }
        memset(hello_array,0,sizeof(hello_array));
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

    //傳送時要先add peer ，盡管是broadcast
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        printf("Malloc peer information fail");
        esp_now_deinit();
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    //複製要加入peer的mac地址  到 peer_addr
    memcpy(peer->peer_addr, broadcast_add, MAC_LEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );

    Mutex = xSemaphoreCreateCounting(1,1);

    //先將自己的mac_address讀取出來******************
    uint8_t self_MAC_ADD[6];
    if(esp_efuse_mac_get_default(self_MAC_ADD) == ESP_OK){
        printf("MAC_ADDRESS of this board is: [%02X:%02X:%02X:%02X:%02X:%02X]\n", self_MAC_ADD[0],self_MAC_ADD[1],self_MAC_ADD[2],self_MAC_ADD[3],self_MAC_ADD[4],self_MAC_ADD[5]);
    }

    route_table_init();
    xTaskCreate(&Task1, "task1", 2048, NULL, 1, NULL); //task 用來定期傳送HELLO訊息
    xTaskCreate(&Task2, "task2", 2048, NULL, 1, NULL);

    printf("start receive**********\n");
    
    //vTaskDelay(1000 / portTICK_PERIOD_MS);
    
}