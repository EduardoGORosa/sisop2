#include "client_actions.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h> 
#include <sys/stat.h> 
#include <dirent.h>   
#include <pthread.h>  
#include <limits.h>   
#include <stdio.h>
#include <errno.h>


#define CHUNK_SIZE MAX_PAYLOAD
static const char* UPLOAD_SUCCESS_MSG = "Arquivo enviado com sucesso.";

int remove_directory_recursively(const char *path) {
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = -1;

    if (d) {
        struct dirent *p;
        r = 0; 
        while (!r && (p = readdir(d))) {
            int r2 = -1;
            char *buf;
            size_t len;

            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
                continue;
            }

            len = path_len + strlen(p->d_name) + 2; 
            buf = malloc(len);

            if (buf) {
                struct stat statbuf;
                snprintf(buf, len, "%s/%s", path, p->d_name);
                if (stat(buf, &statbuf) == 0) { 
                    if (S_ISDIR(statbuf.st_mode)) {
                        r2 = remove_directory_recursively(buf);
                    } else {
                        r2 = remove(buf);
                        if (r2 != 0) {
                            fprintf(stderr, "Erro ao remover arquivo %s: %s\n", buf, strerror(errno));
                        }
                    }
                } else {
                     fprintf(stderr, "Erro ao obter stat de %s: %s\n", buf, strerror(errno));
                     r2 = -1; 
                }
                free(buf);
            } else {
                fprintf(stderr, "Erro ao alocar memória para buffer de caminho.\n");
                r2 = -1; 
            }
            r = r2; 
        }
        closedir(d);
    } else if (errno == ENOENT) { 
        return 0; 
    } else { 
        //fprintf(stderr, "Erro ao abrir diretório %s: %s\n", path, strerror(errno)); // Pode ser muito verboso
        return -1;
    }

    if (r == 0) { 
        if (rmdir(path) != 0) {
            if (errno != ENOENT) { 
                 fprintf(stderr, "Erro ao remover diretório %s: %s\n", path, strerror(errno));
                 r = -1;
            } else {
                r = 0; // Não é erro se já não existe
            }
        }
    }
    return r; 
}

int send_and_wait_ack_client(int s, packet_t *p) {
    int result = -1;
    printf("DEBUG_SWAC: Tentando lock para enviar tipo %d...\n", p->type); fflush(stdout);
    pthread_mutex_lock(&socket_mutex); 
    printf("DEBUG_SWAC: Lock adquirido. Enviando tipo %d...\n", p->type); fflush(stdout);

    if (send_packet(s, p) != 0) {
        fprintf(stderr, "\n[send_and_wait_ack_client] Erro ao enviar pacote tipo %d.\n", p->type);
        fflush(stderr);
    } else {
        printf("DEBUG_SWAC: Pacote tipo %d enviado. Esperando ACK...\n", p->type); fflush(stdout);
        packet_t a;
        if (recv_packet(s, &a) != 0) {
            printf("DEBUG_SWAC: recv_packet falhou ou conexão fechada esperando ACK para tipo %d.\n", p->type); fflush(stdout);
        } else {
            printf("DEBUG_SWAC: Pacote recebido tipo %d (esperando ACK %d) para request tipo %d.\n", a.type, PKT_ACK, p->type); fflush(stdout);
            if (a.type == PKT_ACK) {
                result = 0;
            } else {
                fprintf(stderr, "\n[send_and_wait_ack_client] Resposta inesperada tipo %d para request tipo %d.\n", a.type, p->type);
                fflush(stderr);
            }
        }
    }

    pthread_mutex_unlock(&socket_mutex); 
    printf("DEBUG_SWAC: Mutex liberado. Retornando %d.\n", result); fflush(stdout);
    return result;
}

char* upload_file_action(const char *full_path_arg, int sock) {
    //printf("\nDEBUG: upload_file_action iniciado para '%s'.\n", full_path_arg ? full_path_arg : "NULL"); fflush(stdout);
    char *msg = (char*) malloc(CLIENT_MSG_SIZE);
    if (!msg) { return strdup("Erro: Falha ao alocar memória para mensagem."); }
    msg[0] = '\0';

    if (!full_path_arg || strlen(full_path_arg) == 0) {
        snprintf(msg, CLIENT_MSG_SIZE, "Uso: upload <caminho/completo/do/arquivo.ext>\n");
        //printf("DEBUG: upload_file_action saindo, full_path_arg inválido.\n"); fflush(stdout);
        return msg;
    }

    FILE *fp_check = fopen(full_path_arg, "rb");
    if (fp_check == NULL) {
        snprintf(msg, CLIENT_MSG_SIZE, "Erro ao tentar abrir o arquivo local '%s' para upload (verifique o caminho e permissões).\n", full_path_arg);
        //printf("DEBUG: upload_file_action saindo, fopen falhou para '%s'. Error: %s\n", full_path_arg, strerror(errno)); fflush(stdout);
        return msg;
    }
    fclose(fp_check);

    char *base_filename = strrchr(full_path_arg, '/');
    if (base_filename == NULL) base_filename = (char*)full_path_arg;
    else base_filename++;

    if (strlen(base_filename) == 0) { 
        snprintf(msg, CLIENT_MSG_SIZE, "Erro: Nome do arquivo base resultante é vazio.\n");
        //printf("DEBUG: upload_file_action saindo, nome base vazio.\n"); fflush(stdout);
        return msg;
    }
    if (strlen(base_filename) + 1 > MAX_PAYLOAD) {
        snprintf(msg, CLIENT_MSG_SIZE, "Erro: Nome do arquivo '%s' é muito longo.\n", base_filename);
        //printf("DEBUG: upload_file_action saindo, nome base muito longo.\n"); fflush(stdout);
        return msg;
    }
    
    //printf("DEBUG: upload_file_action: base_filename='%s'. Enviando PKT_UPLOAD_REQ.\n", base_filename); fflush(stdout);
    packet_t rq = { .type = PKT_UPLOAD_REQ, .seq_num = 1 };
    strncpy(rq.payload, base_filename, MAX_PAYLOAD -1);
    rq.payload[MAX_PAYLOAD-1] = '\0';
    rq.payload_size = (uint32_t)strlen(rq.payload) + 1;

    if (send_and_wait_ack_client(sock, &rq) == 0) { 
        //printf("DEBUG: upload_file_action: PKT_UPLOAD_REQ ACKed. Abrindo arquivo para enviar dados.\n"); fflush(stdout);
        FILE *fp = fopen(full_path_arg, "rb"); 
        if (fp == NULL) { 
            snprintf(msg, CLIENT_MSG_SIZE, "Erro crítico: Não foi possível reabrir o arquivo '%s' após ACK do servidor.\n", full_path_arg);
            //printf("DEBUG: upload_file_action saindo, falha ao reabrir arquivo.\n"); fflush(stdout);
            return msg;
        }

        uint32_t seq = 2; char buf[CHUNK_SIZE]; size_t n_read; int error_occurred = 0;
        //printf("DEBUG: upload_file_action: Iniciando envio de chunks.\n"); fflush(stdout);
        while ((n_read = fread(buf, 1, CHUNK_SIZE, fp)) > 0) {
            packet_t dp = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = (uint32_t)n_read };
            memcpy(dp.payload, buf, n_read);
            //printf("DEBUG: upload_file_action: Enviando chunk %u, tamanho %zu.\n", seq, n_read); fflush(stdout);
            if (send_and_wait_ack_client(sock, &dp) != 0) { 
                snprintf(msg, CLIENT_MSG_SIZE, "Erro: Falha ao enviar chunk %u do arquivo ou receber ACK.\n", seq-1);
                //printf("DEBUG: upload_file_action: Erro no send_and_wait_ack para chunk %u.\n", seq-1); fflush(stdout);
                error_occurred = 1; break;
            }
            seq++;
        }
        
        if (!error_occurred && ferror(fp)) { 
            snprintf(msg, CLIENT_MSG_SIZE, "Erro de leitura durante o upload do arquivo '%s'.\n", base_filename);
            //printf("DEBUG: upload_file_action: Erro de ferror().\n"); fflush(stdout);
            error_occurred = 1;
        }
        fclose(fp);

        if (error_occurred) {
            //printf("DEBUG: upload_file_action terminando com erro ocorrido durante envio de chunks.\n"); fflush(stdout);
            return msg;
        }
        
        //printf("DEBUG: upload_file_action: Enviando pacote final de 0 bytes (seq %u).\n", seq); fflush(stdout);
        packet_t endp = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0 };
        pthread_mutex_lock(&socket_mutex);
        int send_final_ok = (send_packet(sock, &endp) == 0);
        pthread_mutex_unlock(&socket_mutex);

        if (!send_final_ok) {
             snprintf(msg, CLIENT_MSG_SIZE, "Erro: Falha ao enviar pacote final de upload para '%s'.\n", base_filename);
             //printf("DEBUG: upload_file_action terminando com erro ao enviar pacote final.\n"); fflush(stdout);
             return msg;
        }
        
        free(msg);
        //printf("DEBUG: upload_file_action terminando com sucesso.\n"); fflush(stdout);
        return (char*)UPLOAD_SUCCESS_MSG;
    } else {
        snprintf(msg, CLIENT_MSG_SIZE, "Erro: Servidor não confirmou o pedido de upload para '%s'.\n", base_filename);
        //printf("DEBUG: upload_file_action: Servidor não ACK UPLOAD_REQ.\n"); fflush(stdout);
    }
    return msg;
}

char* delete_file_action(const char *filename, int sock) {
    printf("DEBUG_DELETE: Iniciando delete_file_action para '%s'\n", filename ? filename : "NULL"); fflush(stdout);
    if (!filename || strlen(filename) == 0) return strdup("Erro: Nome do arquivo para exclusão não especificado.\n");
    
    packet_t rq = { .type = PKT_DELETE_REQ, .seq_num = 1 };
    strncpy(rq.payload, filename, MAX_PAYLOAD -1);
    rq.payload[MAX_PAYLOAD-1] = '\0';
    rq.payload_size = (uint32_t)strlen(rq.payload) + 1;

    printf("DEBUG_DELETE: Chamando send_and_wait_ack_client...\n"); fflush(stdout);
    if (send_and_wait_ack_client(sock, &rq) == 0) { 
        printf("DEBUG_DELETE: send_and_wait_ack_client retornou sucesso.\n"); fflush(stdout);
        return strdup("Solicitação de deleção enviada e confirmada pelo servidor.");
    } else {
        printf("DEBUG_DELETE: send_and_wait_ack_client retornou erro.\n"); fflush(stdout);
        char* msg = (char*)malloc(CLIENT_MSG_SIZE);
        if(msg) snprintf(msg, CLIENT_MSG_SIZE, "Erro: Falha na operação de delete para '%s'.\n", filename);
        else return strdup("Erro na operação de delete e ao alocar msg.");
        return msg;
    }
}

void download_file_action(const char *filename, int sock, const char* initial_cwd) {
    if (!filename || strlen(filename) == 0) { printf("Uso: download <filename.ext>\n"); fflush(stdout); return; }

    packet_t rq = { .type = PKT_DOWNLOAD_REQ, .seq_num = 1 };
    strncpy(rq.payload, filename, MAX_PAYLOAD -1);
    rq.payload[MAX_PAYLOAD-1] = '\0';
    rq.payload_size = (uint32_t)strlen(rq.payload) + 1;

    packet_t r_ack; int initial_req_ok = 0;
    pthread_mutex_lock(&socket_mutex);
    if (send_packet(sock, &rq) == 0) {
        if (recv_packet(sock, &r_ack) == 0 && r_ack.type == PKT_ACK) {
            initial_req_ok = 1;
        } else {
             printf("Erro: Servidor não confirmou o pedido de download para '%s' ou falha na resposta (tipo %d).\n", filename, r_ack.type);
             if(r_ack.type == PKT_NACK) printf("Servidor respondeu com NACK (arquivo pode não existir ou erro no servidor).\n");
        }
    } else printf("Erro ao enviar requisição de download para '%s'.\n", filename);
    pthread_mutex_unlock(&socket_mutex);
    fflush(stdout); 
    if (!initial_req_ok) return;

    char download_path[PATH_MAX];
    snprintf(download_path, PATH_MAX, "%s/%s", initial_cwd, filename);
    FILE *fp = fopen(download_path, "wb");
    if (!fp) { printf("Erro ao abrir o arquivo '%s' (em %s) para escrita.\n", filename, initial_cwd); fflush(stdout); return; }
    printf("Baixando '%s' para '%s'...\n", filename, download_path); fflush(stdout);

    int download_successful = 0; packet_t dp; dp.payload_size = 1; 
    while (dp.payload_size != 0) { 
        int error_in_loop = 0; pthread_mutex_lock(&socket_mutex);
        if (recv_packet(sock, &dp) != 0) { error_in_loop = 1;
        } else {
            if (dp.type != PKT_DOWNLOAD_DATA) { error_in_loop = 1;
            } else {
                if (dp.payload_size == 0) download_successful = 1;
                else {
                    if (fwrite(dp.payload, 1, dp.payload_size, fp) != dp.payload_size) error_in_loop = 1;
                    else {
                        packet_t ca = { .type = PKT_ACK, .seq_num = dp.seq_num, .payload_size = 0 };
                        if (send_packet(sock, &ca) != 0) error_in_loop = 1;
                    }
                }
            }
        }
        pthread_mutex_unlock(&socket_mutex);
        if (error_in_loop || dp.payload_size == 0) break; 
    } 
    fclose(fp);
    if(download_successful) printf("Download de '%s' concluído.\n", filename);
    else { printf("Download de '%s' falhou ou foi incompleto.\n", filename); remove(download_path); }
    fflush(stdout);
}

void list_server_files_action(int sock) {
    //printf("\nDEBUG: Dentro de list_server_files_action(). Tentando mutex...\n"); fflush(stdout);
    packet_t rq = { .type = PKT_LIST_SERVER_REQ, .seq_num = 1, .payload_size = 0 };
    packet_t res; int success = 0;

    pthread_mutex_lock(&socket_mutex);
    //printf("DEBUG: Mutex adquirido em list_server_files_action(). Enviando request...\n"); fflush(stdout);
    if (send_packet(sock, &rq) == 0) {
        //printf("DEBUG: Request enviado. Esperando resposta...\n"); fflush(stdout);
        if (recv_packet(sock, &res) == 0 && res.type == PKT_LIST_SERVER_RES) {
            //printf("DEBUG: Resposta recebida. Tipo: %d, Tamanho: %u\n", res.type, res.payload_size); fflush(stdout);
            success = 1;
        } else {
            //if (res.type != PKT_LIST_SERVER_RES && res.type != 0) { 
            //     printf("DEBUG: Erro ao receber resposta ou tipo inesperado (recebido tipo %d, esperado %d).\n", res.type, PKT_LIST_SERVER_RES); fflush(stdout);
            //} else { 
            //     printf("DEBUG: Falha no recv_packet ao esperar resposta para list_server.\n"); fflush(stdout);
            //}
        }
    } else {
        //printf("DEBUG: Erro ao enviar request PKT_LIST_SERVER_REQ.\n"); fflush(stdout);
    }
    pthread_mutex_unlock(&socket_mutex);
    //printf("DEBUG: Mutex liberado em list_server_files_action().\n"); fflush(stdout);

    if (success) {
        if (res.payload_size > 0) {
            printf("Arquivos no servidor:\n"); fwrite(res.payload, 1, res.payload_size, stdout);
        } else printf("Nenhum arquivo no diretório do servidor ou diretório vazio.\n");
    } else printf("Erro ao obter la lista de arquivos do servidor.\n");
    fflush(stdout);
}

void list_client_files_action(void) {
    DIR *d = opendir(".");
    if (!d) { perror("Erro ao abrir o diretório sync_dir local"); return; }
    printf("Arquivos no diretório sync_dir local:\n");
    struct dirent *e; struct stat st;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (stat(e->d_name, &st) == 0) {
            printf("%s\t%ld bytes\t mtime: %ld atime: %ld ctime: %ld\n",
                   e->d_name, (long)st.st_size, (long)st.st_mtime, (long)st.st_atime, (long)st.st_ctime);
        } else printf("%s\t (não foi possível obter informações)\n", e->d_name);
    }
    closedir(d);
    fflush(stdout);
}

int download_file_to_sync_dir(const char *filename, long expected_size_server, int sock) {
    printf("Sincronizando arquivo do servidor: '%s'\n", filename); fflush(stdout);

    struct stat st;
    long local_size = -1;
    if (stat(filename, &st) == 0) { 
        local_size = (long)st.st_size;
    }

    if (local_size == expected_size_server && expected_size_server != 0) { // Don't skip 0-byte files if server has 0-byte and local doesn't exist
        printf("Arquivo '%s' local já está sincronizado (tamanho: %ld bytes).\n", filename, local_size);
        fflush(stdout);
        return 0; 
    }
    if (local_size == -1 && expected_size_server == 0) { // Server has 0-byte, local doesn't exist -> download 0-byte
         printf("Arquivo '%s' (0 bytes) ausente localmente. Baixando.\n", filename);
    } else {
        printf("Baixando '%s' do servidor (Servidor: %ld bytes, Local: %ld bytes).\n", filename, expected_size_server, local_size);
    }
    fflush(stdout);

    packet_t rq = { .type = PKT_DOWNLOAD_REQ, .seq_num = 1 };
    strncpy(rq.payload, filename, MAX_PAYLOAD -1);
    rq.payload[MAX_PAYLOAD-1] = '\0';
    rq.payload_size = (uint32_t)strlen(rq.payload) + 1;

    packet_t r_ack; int initial_req_ok = 0;
    pthread_mutex_lock(&socket_mutex);
    if (send_packet(sock, &rq) == 0) {
        if (recv_packet(sock, &r_ack) == 0 && r_ack.type == PKT_ACK) {
            initial_req_ok = 1;
        } else {
             fprintf(stderr, "Erro: Servidor não confirmou pedido de download para '%s' (sync) ou falha (tipo %d).\n", filename, r_ack.type);
             if(r_ack.type == PKT_NACK) fprintf(stderr, "Servidor respondeu com NACK (sync).\n");
             fflush(stderr);
        }
    } else {
        fprintf(stderr, "Erro ao enviar requisição de download para '%s' (sync).\n", filename);
        fflush(stderr);
    }
    pthread_mutex_unlock(&socket_mutex);

    if (!initial_req_ok) return -1;

    FILE *fp = fopen(filename, "wb"); 
    if (!fp) {
        fprintf(stderr, "Erro ao abrir o arquivo local '%s' para escrita (sync).\n", filename);
        fflush(stderr);
        return -1;
    }

    int download_successful = 0; packet_t dp; dp.payload_size = 1; 
    long bytes_downloaded = 0;
    while (dp.payload_size != 0) { 
        int error_in_loop = 0; pthread_mutex_lock(&socket_mutex);
        if (recv_packet(sock, &dp) != 0) { error_in_loop = 1;
        } else {
            if (dp.type != PKT_DOWNLOAD_DATA) { error_in_loop = 1;
            } else {
                if (dp.payload_size == 0) download_successful = 1;
                else {
                    if (fwrite(dp.payload, 1, dp.payload_size, fp) != dp.payload_size) error_in_loop = 1;
                    else {
                        bytes_downloaded += dp.payload_size;
                        packet_t ca = { .type = PKT_ACK, .seq_num = dp.seq_num, .payload_size = 0 };
                        if (send_packet(sock, &ca) != 0) error_in_loop = 1;
                    }
                }
            }
        }
        pthread_mutex_unlock(&socket_mutex);
        if (error_in_loop || dp.payload_size == 0) break; 
    } 
    fclose(fp);

    if(download_successful && bytes_downloaded == expected_size_server) {
         printf("Arquivo '%s' sincronizado com sucesso (%ld bytes).\n", filename, bytes_downloaded);
         fflush(stdout);
         return 0;
    } else {
         fprintf(stderr, "Sincronização de '%s' falhou ou incompleta (baixado %ld de %ld bytes).\n", filename, bytes_downloaded, expected_size_server);
         fflush(stderr);
         remove(filename); 
         return -1;
    }
}

int perform_initial_sync(int sock) {
    printf("Iniciando Sincronização Inicial...\n"); fflush(stdout);
    packet_t rq_list = { .type = PKT_LIST_SERVER_REQ, .seq_num = 1, .payload_size = 0 };
    packet_t res_list;
    int success_listing = 0;

    pthread_mutex_lock(&socket_mutex);
    if (send_packet(sock, &rq_list) == 0) {
        if (recv_packet(sock, &res_list) == 0 && res_list.type == PKT_LIST_SERVER_RES) {
            success_listing = 1;
        } else {
            fprintf(stderr, "Falha ao receber lista de arquivos do servidor para sync inicial.\n");
            fflush(stderr);
        }
    } else {
        fprintf(stderr, "Falha ao enviar pedido de lista de arquivos para sync inicial.\n");
        fflush(stderr);
    }
    pthread_mutex_unlock(&socket_mutex);

    if (!success_listing) {
        fprintf(stderr, "Não foi possível obter a lista de arquivos do servidor. Sincronização inicial abortada.\n");
        fflush(stderr);
        return -1;
    }

    if (res_list.payload_size == 0) {
        printf("Servidor não possui arquivos para este usuário. Nada a sincronizar.\n");
        fflush(stdout);
        return 0;
    }

    char *payload_copy = malloc(res_list.payload_size + 1);
    if (!payload_copy) {
        fprintf(stderr, "Falha ao alocar memória para payload_copy em initial_sync.\n");
        return -1;
    }
    memcpy(payload_copy, res_list.payload, res_list.payload_size);
    payload_copy[res_list.payload_size] = '\0'; 

    char *line_saveptr;
    char *line = strtok_r(payload_copy, "\n", &line_saveptr);
    int overall_sync_status = 0; 

    while (line != NULL) {
        char filename[MAX_PAYLOAD / 2]; 
        long size_on_server;
        long mtime_server, atime_server, ctime_server; 

        int items_parsed = sscanf(line, "%[^\t]\t%ld bytes\tmtime:%ld\tatime:%ld\tctime:%ld",
                                  filename, &size_on_server, &mtime_server, &atime_server, &ctime_server);

        if (items_parsed == 5) {
            //printf("Verificando arquivo do servidor: '%s', tamanho: %ld\n", filename, size_on_server);
            //fflush(stdout);
            if (download_file_to_sync_dir(filename, size_on_server, sock) != 0) {
                //fprintf(stderr, "Falha ao sincronizar o arquivo: %s\n", filename);
                //fflush(stderr);
                overall_sync_status = -1; 
            }
        } else {
            fprintf(stderr, "Erro ao parsear linha da lista do servidor: %s\n", line);
            fflush(stderr);
            overall_sync_status = -1;
        }
        line = strtok_r(NULL, "\n", &line_saveptr);
    }
    free(payload_copy);

    if (overall_sync_status == 0) {
        printf("Sincronização inicial de arquivos concluída.\n");
    } else {
        printf("Sincronização inicial de arquivos concluída com uma ou mais falhas.\n");
    }
    fflush(stdout);
    return overall_sync_status;
}
