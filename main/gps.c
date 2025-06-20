#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
void app_main(void)
{

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
    char data[512];
    char *token;
    int length = 0;
    char GPGGA[6] = "GPGGA";
    ESP_ERROR_CHECK(uart_get_buffered_data_len(uart_num, (size_t *)&length));

    while (1)
    {
        uart_flush(UART_NUM_2);
        length = uart_read_bytes(uart_num, data, 512, 80);
        if (length > 0)
        {
            char *result = strstr(data, GPGGA);
            if(result != NULL){
                char *newline = strchr(result, '\n');
                int length_to_next_line = newline - result-1;
                printf("[%.*s]\n",length_to_next_line,result);
            }
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}