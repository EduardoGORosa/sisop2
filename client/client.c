// client/client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include "../common/packet.h"

#define CHUNK_SIZE  MAX_PAYLOAD
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static char last_remote_file[PATH_MAX];
static pthread_mutex_t last_mutex = PTHREAD_MUTEX_INITIALIZER;

int send_and_wait_ack(int sockfd, packet_t *pkt) {
    send_packet(sockfd, pkt);
    packet_t ack;
    return (recv_packet(sockfd,&ack)==0 && ack.type==PKT_ACK) ? 0 : -1;
}

int do_upload(int sockfd, const char *fn) {
    FILE *fp = fopen(fn,"rb"); if(!fp){perror("fopen");return-1;}
    packet_t req={ .type=PKT_UPLOAD_REQ, .seq_num=1, .payload_size=(uint32_t)strlen(fn)+1 };
    memcpy(req.payload,fn,req.payload_size);
    if(send_and_wait_ack(sockfd,&req)<0){fclose(fp);return-1;}
    uint32_t seq=2; char buf[CHUNK_SIZE]; size_t n;
    while((n=fread(buf,1,CHUNK_SIZE,fp))>0){
        packet_t dp={.type=PKT_UPLOAD_DATA,.seq_num=seq++,.payload_size=(uint32_t)n};
        memcpy(dp.payload,buf,n);
        if(send_and_wait_ack(sockfd,&dp)<0) break;
    }
    packet_t endp={.type=PKT_UPLOAD_DATA,.seq_num=seq,.payload_size=0};
    send_packet(sockfd,&endp);
    fclose(fp);
    return 0;
}

int do_delete(int sockfd, const char *fn) {
    packet_t req={ .type=PKT_DELETE_REQ, .seq_num=1, .payload_size=(uint32_t)strlen(fn)+1 };
    memcpy(req.payload,fn,req.payload_size);
    send_packet(sockfd,&req);
    packet_t resp; if(recv_packet(sockfd,&resp)<0) return -1;
    return (resp.type==PKT_ACK)?0:-1;
}

int do_download_manual(int sockfd, const char *fn) {
    // download to ../downloads
    char outdir[PATH_MAX]; getcwd(outdir,sizeof(outdir));
    strcat(outdir,"/downloads"); mkdir(outdir,0755);
    char outpath[PATH_MAX];
    snprintf(outpath,sizeof(outpath),"%s/%s",outdir,fn);

    packet_t req={ .type=PKT_DOWNLOAD_REQ, .seq_num=1, .payload_size=(uint32_t)strlen(fn)+1 };
    memcpy(req.payload,fn,req.payload_size);
    send_packet(sockfd,&req);
    packet_t resp; if(recv_packet(sockfd,&resp)<0||resp.type!=PKT_ACK) return -1;

    FILE *fp=fopen(outpath,"wb"); if(!fp){perror("fopen");return-1;}
    while(1){
        packet_t dp; if(recv_packet(sockfd,&dp)<0||dp.payload_size==0) break;
        fwrite(dp.payload,1,dp.payload_size,fp);
        packet_t ca={ .type=PKT_ACK, .seq_num=dp.seq_num, .payload_size=0 };
        send_packet(sockfd,&ca);
    }
    fclose(fp);
    return 0;
}

int do_download_sync(int sockfd, const char *fn) {
    // download directly into sync_dir (cwd)
    packet_t req={ .type=PKT_DOWNLOAD_REQ, .seq_num=1, .payload_size=(uint32_t)strlen(fn)+1 };
    memcpy(req.payload,fn,req.payload_size);
    send_packet(sockfd,&req);
    packet_t resp; if(recv_packet(sockfd,&resp)<0||resp.type!=PKT_ACK) return -1;

    FILE *fp=fopen(fn,"wb"); if(!fp){perror("fopen");return-1;}
    while(1){
        packet_t dp; if(recv_packet(sockfd,&dp)<0||dp.payload_size==0) break;
        fwrite(dp.payload,1,dp.payload_size,fp);
        packet_t ca={ .type=PKT_ACK, .seq_num=dp.seq_num, .payload_size=0 };
        send_packet(sockfd,&ca);
    }
    fclose(fp);
    return 0;
}

void *sync_apply_thread(void *arg) {
    int sockfd = *(int*)arg;
    while(1) {
        packet_t evt;
        if (recv_packet(sockfd,&evt)<0) break;
        if (evt.type==PKT_SYNC_EVENT) {
            char op = evt.payload[0];
            char fn[PATH_MAX];
            memcpy(fn,evt.payload+1,evt.payload_size-1);
            fn[evt.payload_size-1]='\0';
            pthread_mutex_lock(&last_mutex);
            strcpy(last_remote_file,fn);
            pthread_mutex_unlock(&last_mutex);
            if (op=='U') {
                do_download_sync(sockfd, fn);
            } else if (op=='D') {
                unlink(fn);
            }
        }
    }
    return NULL;
}

void *watch_sync_dir(void *arg) {
    int sockfd = *(int*)arg;
    int fd = inotify_init1(IN_NONBLOCK);
    inotify_add_watch(fd, ".", IN_CREATE|IN_MODIFY|IN_DELETE);
    char buf[BUF_LEN];
    while(1) {
        int len = read(fd,buf,BUF_LEN);
        if (len<0) { usleep(500000); continue; }
        int i=0;
        while (i<len) {
            struct inotify_event *e=(void*)(buf+i);
            if (!(e->mask & IN_ISDIR)) {
                pthread_mutex_lock(&last_mutex);
                int skip = strcmp(last_remote_file,e->name)==0;
                if (skip) last_remote_file[0]='\0';
                pthread_mutex_unlock(&last_mutex);
                if (!skip) {
                    if (e->mask & (IN_CREATE|IN_MODIFY)) do_upload(sockfd,e->name);
                    else if (e->mask & IN_DELETE)       do_delete(sockfd,e->name);
                }
            }
            i += EVENT_SIZE + e->len;
        }
    }
    close(fd);
    return NULL;
}

void usage(void) {
    printf("Comandos:\n");
    printf("  get_sync_dir\n");
    printf("  upload <arquivo>\n");
    printf("  download <arquivo>\n");
    printf("  delete <arquivo>\n");
    printf("  list_server\n");
    printf("  list_client\n");
    printf("  exit\n");
}

int main(int argc,char*argv[]){
    if(argc!=4){fprintf(stderr,"Uso: %s <usuario> <server_ip> <porta>\n",argv[0]);return EXIT_FAILURE;}
    const char *user=argv[1],*ip=argv[2]; int port=atoi(argv[3]);

    mkdir("sync_dir",0755);
    chdir("sync_dir");

    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in serv={.sin_family=AF_INET,.sin_port=htons(port)};
    inet_pton(AF_INET,ip,&serv.sin_addr);
    if(connect(sockfd,(struct sockaddr*)&serv,sizeof(serv))<0){perror("connect");exit(EXIT_FAILURE);}

    packet_t init={.type=PKT_GET_SYNC_DIR,.seq_num=1,.payload_size=(uint32_t)strlen(user)+1};
    memcpy(init.payload,user,init.payload_size);
    send_packet(sockfd,&init);
    packet_t ack; recv_packet(sockfd,&ack);

    pthread_t t1,t2;
    pthread_create(&t1,NULL,watch_sync_dir,&sockfd);
    pthread_create(&t2,NULL,sync_apply_thread,&sockfd);

    usage();
    char line[256];
    while(printf("> "),fgets(line,sizeof(line),stdin)){
        char *cmd=strtok(line," \t\n");
        if(!cmd) continue;
        if(!strcmp(cmd,"get_sync_dir")) {
            char cwd[PATH_MAX]; getcwd(cwd,sizeof(cwd));
            printf("Sync dir local: %s\n",cwd);
        }
        else if(!strcmp(cmd,"upload")) {
            char*fn=strtok(NULL," \t\n"); if(fn) do_upload(sockfd,fn);
        }
        else if(!strcmp(cmd,"download")) {
            char*fn=strtok(NULL," \t\n"); if(fn) do_download_manual(sockfd,fn);
        }
        else if(!strcmp(cmd,"delete")) {
            char*fn=strtok(NULL," \t\n"); if(fn) do_delete(sockfd,fn);
        }
        else if(!strcmp(cmd,"list_server")) {
            packet_t r={.type=PKT_LIST_SERVER_REQ,.seq_num=1,.payload_size=0};
            send_packet(sockfd,&r);
            packet_t res; if(recv_packet(sockfd,&res)==0&&res.type==PKT_LIST_SERVER_RES)
                fwrite(res.payload,1,res.payload_size,stdout);
        }
        else if(!strcmp(cmd,"list_client")) {
            struct stat st; DIR*d=opendir("."); struct dirent*e;
            while((e=readdir(d))) if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
                stat(e->d_name,&st);
                printf("%s\t%ld bytes\tmtime:%ld\tatime:%ld\tctime:%ld\n",
                    e->d_name,(long)st.st_size,(long)st.st_mtime,(long)st.st_atime,(long)st.st_ctime);
            }
            closedir(d);
        }
        else if(!strcmp(cmd,"exit")) break;
        else { printf("Comando desconhecido\n"); usage(); }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}

