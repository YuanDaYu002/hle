#ifndef WRITE_FILE_TO_FLASH_H
#define WRITE_FILE_TO_FLASH_H



#define FLASH_START_ADDRESS 0x100000   //liteOS写OS和app的起始地址
#define ERRASE_SIZE   (7*1024)		   //擦除7M大小的空间

#define READ_BUFFER_SIZE 1024

int  write_file_to_nor_flash(int argc,char**argv);


#endif