#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>

#define SOF_CODE        0x01
#define ACK_CODE        0x06
#define NACK_CODE       0x015
#define REQUEST_CODE    0x00
#define RESPONSE_CODE   0x01

#define RETCODE_OK                  0
#define RETCODE_NOACK               1
#define RETCODE_INVALID_DATALEN     2
#define RETCODE_INVALID_CRC         3
#define RETCODE_INVALID_DATA        4
#define RETCODE_INVALID_SOF         5
#define RETCODE_INVALID_SOF_SIZE    6

#define OUTPUT_BUFFER_SIZE          512

#define ADDITIONAL_SIZE             2
#define ADDITIONAL_SIZE_SL          2
#define ADDITIONAL_HEADSIZE         3


static int debug_flag = 0;
static int callback_flag = 0;
static int max_timeout   = 3;

typedef unsigned char BYTE;

BYTE g_output_buffer[OUTPUT_BUFFER_SIZE];
int           g_seq_number    =     1;

int zio_open(char * name)
{
   return open(name, O_RDWR | O_NOCTTY);
}

void zio_close(int dev)
{
    close(dev);
}

void zio_configure(int dev, int baudRate)
{
    struct termios cfg;
    memset(&cfg, 0, sizeof(cfg));
    
    tcgetattr(dev, &cfg);

    cfsetospeed(&cfg, baudRate);
    cfsetispeed(&cfg, baudRate);
    
    cfg.c_cflag     &=  ~PARENB;            // Make 8n1
    cfg.c_cflag     &=  ~CSTOPB;
    cfg.c_cflag     &=  ~CSIZE;
    cfg.c_cflag     |=  CS8;
    cfg.c_cflag     &=  ~CRTSCTS;           // no flow control

    cfg.c_cc[VMIN] = 1;
    cfg.c_cc[VTIME] = 5;

    cfg.c_cflag     |=  CREAD | CLOCAL;

    cfmakeraw(&cfg);

    //  Flush Port, then applies attributes 
    tcflush( dev, TCIFLUSH );

    if (0 != tcsetattr(dev, TCSANOW, &cfg))
    {
        fprintf (stderr, "Failed to alter device settings\n");
    }
}

int zio_write(int dev, BYTE *buffer, BYTE size)
{
    int result;
    
    result = (int)write(dev, buffer, size);
    
    if (result <= 0)
    {
        fprintf (stderr, "IO Error: write returns %d\n", result);
        return -1;
    }
    
    return result;
}

void printPacket(BYTE * buff, int data_len)
{
    int i;

    for (i=0; i<data_len; i++)
    {
        if (i!=0)
            fprintf(stderr, ",");
        fprintf(stderr,"%x",buff[i]);

    }
}

int zio_read(int dev, BYTE *buffer, BYTE size)
{
    int result;
    int offset = 0;
    long end_time =  (unsigned)time(NULL) + max_timeout;
   
    fd_set set;
    struct timeval tv; // 10ms
    
    while (offset < size)
    {
        FD_ZERO(&set);
        FD_SET(dev, &set);
    
        tv.tv_sec = 0;
        tv.tv_usec = 100*1000;
        if (((unsigned)time(NULL)) >  end_time)
            break;
        result = select(dev+1, &set, NULL, NULL, &tv);
        if (result < 0)
        {
            fprintf (stderr, "IO Error: select fails for device. Result: %d\n", result);
            return 0;
        }
        if (result != 0)
        {
            result = (int)read(dev, buffer + offset, size - offset);
        
            if (result < 0)
            {
                fprintf (stderr, "IO Error: read fails for device. Result: %d\n", result);
                return -1;
            }
            offset += result;
        }
    }
    
    return offset;
}

BYTE crc8_SD(BYTE * buff, int data_len, int offset)
{
    int i;
    BYTE ret = 0xff;

    for (i=0; i<data_len; i++)
    {
        ret = ret ^ buff[offset + i];
        
    }
    
    return ret;
}

void sendAckNack(int fd, BYTE code)
{
    g_output_buffer[0] = code;
    zio_write(fd, g_output_buffer, 1);

    if (debug_flag)
    {
        if (code == ACK_CODE)
            fprintf(stderr, ">> ACK\n");
        else
            fprintf(stderr, ">> NACK\n");
    }
}

void send_SAPIData(int fd, BYTE * buff, int data_len, bool bHaveCallback)
{
    int packet_len = data_len + ADDITIONAL_SIZE;

    if (bHaveCallback)
    {
        g_output_buffer[data_len + ADDITIONAL_HEADSIZE] =   (g_seq_number++) & 0xFF;
        packet_len++;
    }

    g_output_buffer[0] = SOF_CODE;
    g_output_buffer[1] = packet_len & 0xFF;
    g_output_buffer[2] = REQUEST_CODE;

    memcpy(&(g_output_buffer[ADDITIONAL_HEADSIZE]), buff, data_len);

    g_output_buffer[packet_len + 1] = crc8_SD(g_output_buffer, packet_len, 1);

    if (debug_flag)
    {
        fprintf(stderr,"sending >> ");
        printPacket(g_output_buffer, packet_len + 2);
        fprintf(stderr,"\n");
    }
    zio_write(fd, g_output_buffer, packet_len + 2);
}

int receive_SAPIData(int fd, BYTE * receive_buff, BYTE * receive_len)
{
    int bytes_readed = 0;
    int packet_len;
    BYTE crc;

    bytes_readed = zio_read(fd, receive_buff, 2);

    if (bytes_readed != 2)
    {
        return RETCODE_INVALID_SOF_SIZE;
    }

    if (receive_buff[0] != SOF_CODE)
        return RETCODE_INVALID_SOF;

    packet_len = receive_buff[1];
    bytes_readed = zio_read(fd, receive_buff + 2, packet_len);

    if (debug_flag)
    {
        fprintf(stderr,"received << ");
        printPacket(receive_buff, bytes_readed + 2);//packet_len + 2);
        fprintf(stderr,"\n");
    }

    if (bytes_readed != packet_len)
    {
        fprintf(stderr,"received << ");
        sendAckNack(fd, NACK_CODE);
        return RETCODE_INVALID_DATALEN;
    }

    if (crc8_SD(receive_buff, packet_len, 1) != receive_buff[2 + packet_len - 1])
    {

        sendAckNack(fd, NACK_CODE);
        return RETCODE_INVALID_CRC;
    }

    memmove(receive_buff, receive_buff + 2, packet_len-1);
    *receive_len = packet_len-1;
    sendAckNack(fd, ACK_CODE);

    return RETCODE_OK;
}

int send_SAPICommand(int fd, BYTE * cmd_buff, int data_len, BYTE * cmd_response, BYTE * response_len, bool bcallback)
{
    int bytes_readed = 0;
    int packet_len;
    BYTE ack;


    if (data_len > 0)
    {
        send_SAPIData(fd, cmd_buff, data_len, bcallback);

        bytes_readed = zio_read(fd, &ack, 1);
        if ((bytes_readed != 1) || (ack != ACK_CODE))
        {
            if (debug_flag)
            {
                fprintf(stderr,"%x instead ACK \n", ack);
            }
            return RETCODE_NOACK;
        }
        if (debug_flag)
        {
            fprintf(stderr,"<< ACK \n");
        }

         if (cmd_response == NULL)
            return RETCODE_OK;
    }

    return receive_SAPIData(fd, cmd_response, response_len);
}   

bool isHexDigit(char digit)
{
    return ('0' <= digit && digit <= '9') || ('a' <= digit && digit <= 'f') || ('A' <= digit && digit <= 'F');
}

BYTE hexCharToByte(char digit)
{
    if (('A' <= digit && digit <= 'F'))
        return (digit - 'A' + 10);
    return ('0' <= digit && digit <= '9') ? (digit - '0') : (digit - 'a' + 10);
}

int hexStrToBA(char * hexStr, BYTE * val)
{
    BYTE count = 0;
    int i = 0;
    
    while (i < strlen(hexStr))
    {
        if (hexStr[i] == ' ')
        {
            i++;
            continue;
        }
        
        if (isHexDigit(hexStr[i]))
        {
            if ((i + 1) < strlen(hexStr) && isHexDigit(hexStr[i+1]))
            {
                if ((i + 2) < strlen(hexStr) && hexStr[i+2] != ' ')
                    printf("paring hex array (three subsequent digits): %s\n", hexStr);
                val[count++] = hexCharToByte(hexStr[i]) * 16 + hexCharToByte(hexStr[i+1]);
                i+=2;
            }
            else {
                val[count++] = hexCharToByte(hexStr[i]);
                i++;
            }
        } else
            printf("paring hex array (bad char): %s\n", hexStr);
    }
    return count;
}

static struct option long_options[] =
{
    {"buffer", required_argument, NULL, 'b'},
    {"port", required_argument, NULL, 'p'},
    {"responce_counter", required_argument,  NULL, 'r'},
    {"timeout", required_argument, NULL, 't'},

    {"debug", no_argument,       &debug_flag, 1},
    {"callback", no_argument,       &callback_flag, 1},
    
    {NULL, 0, NULL, 0}
};

#define CMD_SEND_DATA       0x01
#define CMD_RECEIVE_REQ     0x02

void printResponse(BYTE * buff,int num, int data_len)
{
    int i;

    printf("RESP %d {",num);
    for (i=0; i<data_len; i++)
    {
        //if (i!=0)
        // fprintf(stdout, " ");
        fprintf(stdout," %02x",buff[i]);

    }
    fprintf(stdout," }\n");
}

void printUsage()
{
     fprintf(stderr, "Wrong usage of this utility.\n Right format is: uSAPI -b <byte_array> -p <devname> [-r <number_of_waiting responces>] [--callback] [--debug]\n");               
}

int main (int argc, char **argv)
{
    char *portname;

    static BYTE out_rawdata[300];
    static BYTE  out_data_len = 0;
    static BYTE in_rawdata[300];
    static BYTE  in_data_len = 0;
    static int  fd;

    static int num_responses_we_need = 0;
    static int ret_code = 0;
    static int opt;
    static int longIndex;

    if (argc < 3)
    {
        printUsage();
        exit(-100);
    }
    
    while (1)
    {
        opt = getopt_long( argc, argv, "b:p:r:t:", long_options, NULL);
        if (opt == -1)
            break;
        switch( opt )
        {
            case 'b':
                out_data_len = hexStrToBA(optarg, out_rawdata);
                break;
                
            case 'p':
                portname = optarg;
                break;

            case 'r':
                num_responses_we_need = atoi(optarg);
                break;
                
            case 't':
                max_timeout = atoi(optarg);
                break;

            case 0:     /* длинная опция без короткого эквивалента */
                break;

            case '?':
                printUsage();
                 /* getopt_long already printed an error message. */
                exit(-100); 
                break;
                
            default:
                /* сюда попасть невозможно. */
                break;
        }
    }
    
    fd = zio_open(portname);
    if (fd < 0)
    {
        fprintf(stderr,"error %d opening %s: %s\n", errno, portname, strerror (errno));
        return -10;
    }
    zio_configure(fd, B115200);

    if (num_responses_we_need == 0)
    {
        ret_code = send_SAPICommand(fd, out_rawdata, out_data_len,  NULL, NULL, 0);
        zio_close(fd);
        exit(ret_code);
    }
    else
    {
        BYTE resp_counter = 1;
        ret_code = send_SAPICommand(fd, out_rawdata, out_data_len,  in_rawdata, &in_data_len, callback_flag);
        if (ret_code != 0)
        {
            zio_close(fd);
            exit(ret_code);
        }
        printResponse(in_rawdata, 0, in_data_len);

        num_responses_we_need --;

        while (num_responses_we_need)
        {
            ret_code = receive_SAPIData(fd,  in_rawdata, &in_data_len);
            if (ret_code != 0)
            {
                zio_close(fd);
                exit(ret_code);
            }
            printResponse(in_rawdata, resp_counter, in_data_len);

            num_responses_we_need --;
            resp_counter ++;
        }

    }

    zio_close(fd);
    exit(0);
    return 0;
}
