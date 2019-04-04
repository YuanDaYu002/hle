 
/***************************************************************************
* @file: http.h 
* @author:   
* @date:  4,4,2019
* @brief:  
* @attention:
***************************************************************************/

#ifndef _HTTP_H
#define _HTTP_H

/*******************************************************************************
*@ Description    :  http发送post请求
*@ Input          :<host_url> 远端服务器的URL
					<post_str>需要推送的字符串数据（JSON格式）
*@ Output         :
*@ Return         :成功：服务器的响应数据 失败：NULL
*@ Attention	  :返回的指针使用后需要free
*******************************************************************************/
char* http_post(char *host_url,char*post_str);

/*******************************************************************************
*@ Description    :HTTP 下载文件
*@ Input          :<url> : 要下载的文件对应的URL
*@ Output         :
*@ Return         :成功：下载的文件绝对路径 字符串指针 
*          			失败：NULL
*@ attention      :返回的指针使用后需要free
*******************************************************************************/
char * http_dowload_file(char* url);




#endif


