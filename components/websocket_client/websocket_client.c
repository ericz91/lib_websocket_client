/*
 * websocket_client.c
 *
 *  Created on: 2018年6月24日
 *      Author: ZLJ DJB
 */

#include "websocket_client.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "math.h"

#include "stdio.h"

#include <errno.h>

#include "base64.h"
#include "sha1.h"

const char* TAG = "client_setup";

const char* PROTOCOL_HEAD_TEMPLATE = "GET %s HTTP/1.1\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Host: %s"
CRLF
"Sec-WebSocket-Key: %s"
CRLF
"Sec-WebSocket-Protocol: %s"
CRLF
"Sec-WebSocket-Version: 13\r\n"
CRLF;

//数据发送队列
QueueHandle_t sendDataQueue;
QueueHandle_t recvDataQueue;

/************************
 * 数据交互 发送binary数据
 */
esp_err_t send_binary_data(web_socket_ctx* ctx, uint8_t* data,
		uint16_t total_length) {

	web_socket_data_package *package =
			(web_socket_data_package*) malloc_and_reset(
					sizeof(web_socket_data_package));

	package->socket_id = ctx->socket_id;
	package->is_binary = 1;
	package->len = total_length;
	package->data = data;

	if (sendDataQueue != NULL) {

		xQueueSendToBack(sendDataQueue, package, 500/portTICK_RATE_MS);
		return ESP_OK;
	} else {
		return ESP_FAIL;
	}

}

/***********************
 * 数据交互 发送str数据
 */
esp_err_t send_string_data(web_socket_ctx* ctx, char* data,
		uint16_t total_length) {

	web_socket_data_package *package =
			(web_socket_data_package*) malloc_and_reset(
					sizeof(web_socket_data_package));

	package->socket_id = ctx->socket_id;
	package->is_binary = 0;
	package->len = total_length;
	package->data = data;

	if (sendDataQueue != NULL) {

		xQueueSendToBack(sendDataQueue, package, 500/portTICK_RATE_MS);
		return ESP_OK;
	} else {
		return ESP_FAIL;
	}

}

/*******************
 * 数据发送任务队列
 *******/
void web_socket_send_task(void *param) {

	sendDataQueue = xQueueCreate(SEND_DATA_QUEUE_DEPTH,
			sizeof(web_socket_data_package));
	while (1) {

		web_socket_data_package* data_package =
				(web_socket_data_package*) malloc_and_reset(
						sizeof(web_socket_data_package));
		int xstatus = xQueueReceive(sendDataQueue, data_package,
				1000 /portTICK_RATE_MS);
		if (xstatus != pdPASS)
			continue;

		//send(data_package.socket_id, data_package.data, data_package.len, 0);
		send_data(data_package);
		free(data_package);

		vTaskDelay(10 / portTICK_PERIOD_MS);
	}

}

void send_data(web_socket_data_package* package) {

	uint8_t mask[4];

	int socket_id = package->socket_id;

	uint16_t len = package->len;

	uint8_t is_binary = package->is_binary;

	uint8_t *data = (uint8_t*) (package->data);

	if (package->is_binary) {
		send_char(socket_id, (uint8_t) (WS_FIN | WS_OPCODE_BINARY));
	} else {
		send_char(socket_id, (uint8_t) (WS_FIN | WS_OPCODE_TEXT));
	}

	if (len > 125) {
		send_char(socket_id, (uint8_t) (WS_SIZE16 | WS_MASK));
		send_char(socket_id, (uint8_t) (len >> 8));
		send_char(socket_id, (uint8_t) (len & 0xff));
	} else {
		send_char(socket_id, (uint8_t) (len | WS_MASK));
	}

	mask[0] = (uint8_t) (rand() % 255);
	mask[1] = (uint8_t) (rand() % 255);
	mask[2] = (uint8_t) (rand() % 255);
	mask[3] = (uint8_t) (rand() % 255);

	send(socket_id, mask, sizeof(uint8_t) * 4, 0);

	for (int i = 0; i < len; ++i) {
		uint8_t tmp = data[i];
		send_char(socket_id, tmp ^ mask[i % 4]);
	}

}

/******************
 * 数据接收任务
 *****************/

void* recv_handler_out_queue(QueueHandle_t *queue) {

	web_socket_recv_handler* handler =
			(web_socket_recv_handler*) malloc_and_reset(
					sizeof(web_socket_recv_handler));

	int xstatus = xQueueReceive(queue, handler, 100 /portTICK_RATE_MS);

	if (xstatus == pdPASS) {
		return handler;
	} else {
		free(handler);
		return NULL;
	}
}

uint8_t non_blocking_recv_char(int socket_id, uint8_t* data) {

	int len = recv(socket_id, data, sizeof(uint8_t) * 1, MSG_DONTWAIT);

	return len;
}

int8_t handle_stream(web_socket_recv_handler *handler) {


	uint8_t rec_data;


	int8_t rec_len = non_blocking_recv_char(handler->socket_id, &rec_data);
	if (rec_len == 0)
		return 0;

	else if (rec_len < 0)
		return -1;


	uint8_t msgtype;
	uint8_t bite;
	uint16_t length=0;
	uint8_t mask[4];
	uint8_t index;
	unsigned int i;
	bool hasMask = false;


	msgtype = rec_data;

	rec_len = non_blocking_recv_char(handler->socket_id, &rec_data);
	if (rec_len == 0)
		return 0;

	else if (rec_len < 0)
		return -1;


	if (rec_data & WS_MASK) {
		hasMask = true;
		length = rec_data & ~WS_MASK;
	} else {
		length = rec_data ;
	}



	index = 6;


	if (length == WS_SIZE16) {
		uint8_t data;
		rec_len = non_blocking_recv_char(handler->socket_id, &data);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

		length = data << 8;

		rec_len = non_blocking_recv_char(handler->socket_id, &data);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

		length |= data;
	} else if (length == WS_SIZE64) {
		return -2;
	}


	if (hasMask) {

		rec_len = non_blocking_recv_char(handler->socket_id, mask[0]);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

		rec_len = non_blocking_recv_char(handler->socket_id, mask[1]);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

		rec_len = non_blocking_recv_char(handler->socket_id, mask[2]);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

		rec_len = non_blocking_recv_char(handler->socket_id, mask[3]);
		if (rec_len == 0)
			return 0;

		else if (rec_len > 127)
			return -1;

	}

	uint8_t opcode = msgtype & ~WS_FIN;


	if (opcode == WS_OPCODE_CLOSE) {
		return -3;
	} else if (opcode == WS_OPCODE_PING) {

		int ret = send_char(handler->socket_id, WS_FIN | WS_OPCODE_PONG);
		if (ret < 1)
			return -1;
		else
			return 1;
	}


	web_socket_data_package* package =(web_socket_data_package*) malloc_and_reset(sizeof(web_socket_data_package));


	uint8_t* data;
	if(opcode == WS_OPCODE_TEXT){
		data = (uint8_t*)malloc_and_reset(sizeof(uint8_t) * (length+1));
		data[length] = '\0';
	}
	else
		data = (uint8_t*)malloc_and_reset(sizeof(uint8_t) * length);


	rec_data = recv(handler->socket_id, data, sizeof(uint8_t) * length,MSG_WAITALL);



	if (rec_data < 0)
		return -1;

	package->data = data;
	package->is_binary = opcode == WS_OPCODE_BINARY ? 1 : 0;
	package->len = length;

	package->socket_id = handler->socket_id;

	handler->recv_callback(package);
	free(package);



	return 1;

}

xListItem* get_list_next(xList *pxList) {

	xList * const pxConstList = pxList;
	pxConstList->pxIndex = pxConstList->pxIndex->pxNext;
	if (pxConstList->pxIndex == ( xListItem *) &(pxConstList->xListEnd)) {
		pxConstList->pxIndex = pxConstList->pxIndex->pxNext;
	}
	return pxConstList->pxIndex;

}

esp_err_t add_recv_task(web_socket_recv_handler* handler) {
	if (recvDataQueue != NULL) {

		xQueueSendToBack(recvDataQueue, handler, 500/portTICK_RATE_MS);

		return ESP_OK;
	} else {
		return ESP_FAIL;
	}
}

void web_socket_recv_task(void* params) {

	recvDataQueue = xQueueCreate(RECV_DATA_QUEUE_DEPTH,
			sizeof(web_socket_recv_handler));

	xList *handler_list = (xList*) malloc_and_reset(sizeof(xList));

	vListInitialise(handler_list);

	while (1) {

		web_socket_recv_handler* handler = recv_handler_out_queue(
				recvDataQueue);

		xListItem* handler_tmp = NULL;

		if (handler == NULL) {

			goto handle_rec;
		}

		xListItem *item = (xListItem*) malloc_and_reset(sizeof(xListItem));

		vListInitialiseItem(item);

		item->pvOwner = handler;

		vListInsertEnd(handler_list, item);

		handle_rec:

		handler_tmp = get_list_next(handler_list);

		if (handler_tmp == ( xListItem *) &(handler_list->xListEnd)) {
			continue;
		}


		int ret = handle_stream(
				(web_socket_recv_handler*) (handler_tmp->pvOwner));
//		if (ret < 0) {
//			uxListRemove(handler);
//		}

		vTaskDelay(20 / portTICK_PERIOD_MS);

	}

}

/*****************
 * 连接tcp和握手协议
 */
esp_err_t connect_websocket(web_socket_info *info, web_socket_ctx* ctx) {
	int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_socket < 0)
		return ESP_FAIL;

	if (connect(tcp_socket, &(info->server_addr), sizeof(info->server_addr)))
		return ESP_FAIL;

	if (hand_shake(tcp_socket, info->path,
			inet_ntoa(info->server_addr.sin_addr))) {

		ctx->socket_id = tcp_socket;
		ws_conn_state state = CONNECTED;
		ctx->state = state;
		return ESP_OK;

	} else {
		close(tcp_socket);
		return ESP_FAIL;
	}

}

bool hand_shake(int socket_id, char* path, char* host) {

	return analyze_request(socket_id, path, host);

}

bool analyze_request(int socket_id, char* path, char* host) {

	int bite;
	bool foundupgrade = false;
	unsigned long intkey[2];
	char* serverKey;
	char keyStart[17];
	char b64Key[25];
	char key[25];
	char *protocol_head = malloc_and_reset(sizeof(char) * 256);

	for (int i = 0; i < 16; i++) {
		keyStart[i] = (char) rand() % 255;
	}

	base64_encode(b64Key, keyStart, 16);

	for (int i = 0; i < 24; i++) {
		key[i] = b64Key[i];

	}

	char* procotol = "";

	sprintf(protocol_head, PROTOCOL_HEAD_TEMPLATE, path, host, key, procotol);

	send(socket_id, protocol_head, strlen(protocol_head), 0);

	free(protocol_head);

	char* data_recv = (char*) malloc_and_reset(sizeof(char) * 350);
	data_recv[349] = '\0';
	recv(socket_id, data_recv, sizeof(char) * 349, 0);
	char* server_key_pre = strstr(data_recv, "Sec-WebSocket-Accept: ");

	char* server_key = sub_string(server_key_pre, 23, 50);

	free(data_recv);

	strcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

	uint8_t *hash;
	char result[21];
	char b64Result[30];

	SHA1_CTX1 ctx;

	SHA1Init1(&ctx);

	SHA11(result, key, strlen(key));

	result[20] = '\0';

	base64_encode(b64Result, result, 20);

	int ret = strcmp(b64Result, server_key);

	return ret == 0;
}

int split_string(char* target, char* split, char** targetArray, int* count) {
	char* p = NULL, *pTmp = NULL;
	int tempCount = 0;

	p = target;
	pTmp = target;

	while (*p != '\0') {
		p = strchr(p, split); //检查符合条件的位置p 后移
		if (p != NULL) {
			if (p - pTmp > 0) //算出符合条件具体的指针位置
					{
				strncpy(targetArray[tempCount], pTmp, p - pTmp);
				//ESP_LOGI(TAG,"starting client task 9 %s", targetArray[tempCount]);
				targetArray[tempCount][p - pTmp] = '\0'; //变成C风格的字符串
				tempCount++;
				pTmp = p = p + 1;
			}
		} else {
			break;
		}

	}
	*count = tempCount; //具体有几个分割的字符串
//ESP_LOGI(TAG,"starting client task 9 len %d", tempCount);
	return 0;
}

void* malloc_and_reset(size_t size) {
	void* p = malloc(size);
	memset(p, 0, size);
	return p;
}

char* sub_string(char* dest, int start, int end) {
	char* p = malloc_and_reset(sizeof(char) * (end - start + 2));

	for (int i = 0; i < end - start + 1; i++) {
		p[i] = dest[start - 1 + i];
	}
	p[end - start + 1] = '\0';
	return p;
}

uint8_t send_char(int socket_id, uint8_t data) {
	uint8_t tmp = data;
	return send(socket_id, &tmp, 1, MSG_DONTWAIT);
}

