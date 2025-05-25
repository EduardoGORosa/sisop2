#include "server_request_handler.h"
#include "server_utils.h" // For mkdir_p, send_and_wait_ack_server
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For remove, close
#include <sys/stat.h>   // For stat
#include <dirent.h>     // For opendir, readdir, closedir

#define CHUNK_SIZE MAX_PAYLOAD


// Helper function to propagate a file to other connected devices of the same user
void propagate_file_to_other_devices(UserSession_t *user_session, const char *base_filename, const char *full_file_path_on_server, int originating_conn_fd) {
    if (!user_session || !base_filename || !full_file_path_on_server) return;

    FILE *f_to_propagate = fopen(full_file_path_on_server, "rb");
    if (!f_to_propagate) {
        perror("propagate_file: fopen failed");
        return;
    }

    printf("Propagando arquivo '%s' para outros dispositivos do usuário '%s'.\n", base_filename, user_session->username);

    lock_sessions(); // Lock before iterating connection_fds
    for (int i = 0; i < MAX_SESSIONS_PER_USER; i++) {
        int other_fd = user_session->connection_fds[i];
        if (other_fd > 0 && other_fd != originating_conn_fd) { // If connection active and not the source
            printf("  Enviando '%s' para fd=%d\n", base_filename, other_fd);
            
            packet_t req_pkt = { .type = PKT_UPLOAD_REQ, .seq_num = 1 }; // Server initiates "upload" to other client
            strncpy(req_pkt.payload, base_filename, MAX_PAYLOAD -1);
            req_pkt.payload[MAX_PAYLOAD-1] = '\0';
            req_pkt.payload_size = (uint32_t)strlen(base_filename) + 1;

            if (send_and_wait_ack_server(other_fd, &req_pkt) == 0) {
                rewind(f_to_propagate); // Rewind file for each client
                uint32_t seq = 2;
                char buf[CHUNK_SIZE];
                size_t n_read;
                while ((n_read = fread(buf, 1, CHUNK_SIZE, f_to_propagate)) > 0) {
                    packet_t data_pkt = { .type = PKT_UPLOAD_DATA, .seq_num = seq++, .payload_size = (uint32_t)n_read };
                    memcpy(data_pkt.payload, buf, n_read);
                    if (send_and_wait_ack_server(other_fd, &data_pkt) != 0) {
                        fprintf(stderr, "Erro ao propagar chunk de '%s' para fd=%d. Interrompendo para este fd.\n", base_filename, other_fd);
                        break; 
                    }
                }
                if (ferror(f_to_propagate)) {
                     fprintf(stderr, "Erro de leitura ao propagar '%s' para fd=%d.\n", base_filename, other_fd);
                } else {
                    // Send final 0-byte packet
                    packet_t end_pkt = { .type = PKT_UPLOAD_DATA, .seq_num = seq, .payload_size = 0 };
                    send_packet(other_fd, &end_pkt); // Client doesn't ACK this one, just closes file
                    printf("  Propagação de '%s' para fd=%d concluída (ou erro no envio final).\n", base_filename, other_fd);
                }
            } else {
                fprintf(stderr, "Cliente fd=%d não confirmou UPLOAD_REQ para propagação de '%s'.\n", other_fd, base_filename);
            }
        }
    }
    unlock_sessions();
    fclose(f_to_propagate);
}

// Helper function to propagate delete to other connected devices
void propagate_delete_to_other_devices(UserSession_t *user_session, const char *base_filename, int originating_conn_fd) {
    if (!user_session || !base_filename) return;

    printf("Propagando deleção do arquivo '%s' para outros dispositivos do usuário '%s'.\n", base_filename, user_session->username);

    lock_sessions();
    for (int i = 0; i < MAX_SESSIONS_PER_USER; i++) {
        int other_fd = user_session->connection_fds[i];
        if (other_fd > 0 && other_fd != originating_conn_fd) {
            printf("  Enviando pedido de DELETE para '%s' para fd=%d\n", base_filename, other_fd);
            
            packet_t del_pkt = { .type = PKT_DELETE_REQ, .seq_num = 1 };
            strncpy(del_pkt.payload, base_filename, MAX_PAYLOAD -1);
            del_pkt.payload[MAX_PAYLOAD-1] = '\0';
            del_pkt.payload_size = (uint32_t)strlen(base_filename) + 1;

            // Client is expected to ACK this delete request.
            if (send_and_wait_ack_server(other_fd, &del_pkt) != 0) {
                fprintf(stderr, "Cliente fd=%d não confirmou DELETE_REQ para '%s'.\n", other_fd, base_filename);
            } else {
                 printf("  Cliente fd=%d confirmou DELETE_REQ para '%s'.\n", other_fd, base_filename);
            }
        }
    }
    unlock_sessions();
}


void handle_received_packet(int client_conn_fd, packet_t *pkt, UserSession_t *user_session, const char *user_storage_base_dir) {
    char filename_from_payload[MAX_PAYLOAD + 1];
    if (pkt->payload_size > 0 && pkt->payload_size <= MAX_PAYLOAD) {
        memcpy(filename_from_payload, pkt->payload, pkt->payload_size);
        // Ensure null termination, especially if payload_size doesn't include it
        // or if it's exactly MAX_PAYLOAD.
        if (pkt->payload_size == MAX_PAYLOAD) {
             filename_from_payload[MAX_PAYLOAD -1] = '\0'; // Force null if full
        } else {
             filename_from_payload[pkt->payload_size > 0 ? pkt->payload_size-1 : 0] = '\0'; // Common case for C-strings
             // If payload_size truly is strlen+1, then payload[payload_size-1] is already '\0'.
             // For safety, if payload_size is just length, ensure termination:
             filename_from_payload[pkt->payload_size] = '\0';
        }
    } else {
        filename_from_payload[0] = '\0';
    }


    char full_path_on_server[PATH_MAX];
    if (filename_from_payload[0] != '\0') { // Only construct if filename is present
        snprintf(full_path_on_server, sizeof(full_path_on_server), "%s/%s", user_storage_base_dir, filename_from_payload);
    } else {
        full_path_on_server[0] = '\0'; // No filename, no path
    }


    switch (pkt->type) {
        case PKT_UPLOAD_REQ: {
            printf("[*] Upload Req: '%s' from user '%s' (fd=%d)\n", filename_from_payload, user_session->username, client_conn_fd);
            if (full_path_on_server[0] == '\0') {
                fprintf(stderr, "Erro: Nome de arquivo inválido ou ausente para PKT_UPLOAD_REQ.\n");
                packet_t nack_resp = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &nack_resp);
                break;
            }

            FILE *f_upload = fopen(full_path_on_server, "wb");
            if (!f_upload) {
                perror("fopen for upload failed");
                packet_t nack_resp = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &nack_resp);
                break;
            }

            packet_t ack_resp = { .type = PKT_ACK, .seq_num = pkt->seq_num, .payload_size = 0 };
            send_packet(client_conn_fd, &ack_resp); // ACK the UPLOAD_REQ

            packet_t data_pkt;
            while (recv_packet(client_conn_fd, &data_pkt) == 0 && data_pkt.type == PKT_UPLOAD_DATA) {
                if (data_pkt.payload_size == 0) { // End of transfer from this client
                    packet_t final_ack = { .type = PKT_ACK, .seq_num = data_pkt.seq_num, .payload_size = 0 };
                    // Original client doesn't wait for this ACK based on its logic, but sending it is fine.
                    // Or, per client logic, no ACK for 0-byte packet.
                    // However, the client's send_and_wait_ack_client IS used for data chunks,
                    // so the server MUST ACK data chunks. The 0-byte end packet is sent without send_and_wait_ack.
                    // The loop condition `pkt.payload_size > 0` in original server.c meant it didn't ACK the 0-byte one.
                    // Let's stick to server not ACKing the 0-byte one as it exits loop.
                    break; 
                }
                fwrite(data_pkt.payload, 1, data_pkt.payload_size, f_upload);
                packet_t chunk_ack = { .type = PKT_ACK, .seq_num = data_pkt.seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &chunk_ack);
            }
            fclose(f_upload);
            printf("[*] Upload completed for: '%s'\n", filename_from_payload);

            // Propagate to other devices
            propagate_file_to_other_devices(user_session, filename_from_payload, full_path_on_server, client_conn_fd);
            break;
        }
        case PKT_DOWNLOAD_REQ: {
            printf("[*] Download Req: '%s' for user '%s' (fd=%d)\n", filename_from_payload, user_session->username, client_conn_fd);
             if (full_path_on_server[0] == '\0') {
                fprintf(stderr, "Erro: Nome de arquivo inválido ou ausente para PKT_DOWNLOAD_REQ.\n");
                packet_t nack_resp = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &nack_resp);
                break;
            }

            FILE *f_download = fopen(full_path_on_server, "rb");
            if (!f_download) {
                perror("fopen for download failed");
                packet_t nack_resp = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                // snprintf(nack_resp.payload, MAX_PAYLOAD, "File not found or access denied.");
                // nack_resp.payload_size = strlen(nack_resp.payload) + 1;
                send_packet(client_conn_fd, &nack_resp);
                break;
            }

            packet_t ack_resp = { .type = PKT_ACK, .seq_num = pkt->seq_num, .payload_size = 0 };
            send_packet(client_conn_fd, &ack_resp); // ACK the DOWNLOAD_REQ

            uint32_t seq = 1; // Sequence for data packets
            char buf[CHUNK_SIZE];
            size_t n_read;
            packet_t file_data_pkt;

            while ((n_read = fread(buf, 1, CHUNK_SIZE, f_download)) > 0) {
                file_data_pkt.type = PKT_DOWNLOAD_DATA;
                file_data_pkt.seq_num = seq++;
                file_data_pkt.payload_size = (uint32_t)n_read;
                memcpy(file_data_pkt.payload, buf, n_read);
                if (send_and_wait_ack_server(client_conn_fd, &file_data_pkt) != 0) {
                    fprintf(stderr, "Erro: Cliente não confirmou recebimento de chunk para '%s'.\n", filename_from_payload);
                    break; // Stop sending if client doesn't ACK
                }
            }
            fclose(f_download);

            if (ferror(f_download)){
                 fprintf(stderr, "Erro de leitura durante download de '%s'.\n", filename_from_payload);
            } else {
                 // Send final 0-byte packet to indicate end of transfer
                file_data_pkt.type = PKT_DOWNLOAD_DATA;
                file_data_pkt.seq_num = seq;
                file_data_pkt.payload_size = 0;
                send_packet(client_conn_fd, &file_data_pkt); // Client expects this, doesn't ACK it
                printf("[*] Download data sent for: '%s'\n", filename_from_payload);
            }
            break;
        }
        case PKT_DELETE_REQ: {
            printf("[*] Delete Req: '%s' for user '%s' (fd=%d)\n", filename_from_payload, user_session->username, client_conn_fd);
            if (full_path_on_server[0] == '\0') {
                fprintf(stderr, "Erro: Nome de arquivo inválido ou ausente para PKT_DELETE_REQ.\n");
                packet_t nack_resp = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &nack_resp); // Envia NACK para o cliente que requisitou
                break;
            }

            packet_t resp_pkt_to_originating_client; // Pacote de resposta para o cliente A
            resp_pkt_to_originating_client.seq_num = pkt->seq_num;
            resp_pkt_to_originating_client.payload_size = 0;

            if (remove(full_path_on_server) == 0) {
                printf("Arquivo '%s' removido do servidor.\n", full_path_on_server);
                resp_pkt_to_originating_client.type = PKT_ACK;
                
                // Envia o ACK para o cliente solicitante ANTES de propagar
                send_packet(client_conn_fd, &resp_pkt_to_originating_client);
                printf("[*] ACK enviado para fd=%d para PKT_DELETE_REQ de '%s'.\n", client_conn_fd, filename_from_payload);

                // Agora, tenta propagar a deleção para outros dispositivos.
                // A falha aqui não afetará a resposta já enviada ao cliente original.
                propagate_delete_to_other_devices(user_session, filename_from_payload, client_conn_fd);

            } else {
                perror("remove failed on server for PKT_DELETE_REQ");
                resp_pkt_to_originating_client.type = PKT_NACK;
                // Envia o NACK para o cliente solicitante
                send_packet(client_conn_fd, &resp_pkt_to_originating_client);
                printf("[*] NACK enviado para fd=%d para PKT_DELETE_REQ de '%s'.\n", client_conn_fd, filename_from_payload);
            }
            // Não há mais send_packet aqui, já foi feito acima.
            break;
        }        
        case PKT_LIST_SERVER_REQ: {
            printf("[*] List Server Req from user '%s' (fd=%d)\n", user_session->username, client_conn_fd);
            DIR *d = opendir(user_storage_base_dir);
            if (!d) {
                perror("opendir for list_server failed");
                packet_t nack_res = { .type = PKT_NACK, .seq_num = pkt->seq_num, .payload_size = 0 };
                send_packet(client_conn_fd, &nack_res);
                break;
            }

            char list_buf[CHUNK_SIZE]; // Can be larger if needed, or send in multiple packets
            size_t offset = 0;
            struct dirent *entry;
            struct stat st;

            while ((entry = readdir(d)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char entry_full_path[PATH_MAX];
                snprintf(entry_full_path, sizeof(entry_full_path), "%s/%s", user_storage_base_dir, entry->d_name);
                if (stat(entry_full_path, &st) < 0) {
                    perror("stat for list_server entry failed");
                    continue;
                }
                // Append entry info to list_buf
                offset += snprintf(list_buf + offset, CHUNK_SIZE - offset,
                                   "%s\t%ld bytes\tmtime:%ld\tatime:%ld\tctime:%ld\n",
                                   entry->d_name, (long)st.st_size,
                                   (long)st.st_mtime, (long)st.st_atime, (long)st.st_ctime);
                if (offset >= CHUNK_SIZE - 200) { // Heuristic: leave some space for one more small entry + null
                    // Buffer full, send what we have or implement multi-packet response
                    fprintf(stderr, "Aviso: Buffer de list_server cheio. Lista pode estar truncada.\n");
                    break;
                }
            }
            closedir(d);

            packet_t list_res = { .type = PKT_LIST_SERVER_RES, .seq_num = pkt->seq_num, .payload_size = (uint32_t)offset };
            if (offset > 0) {
                memcpy(list_res.payload, list_buf, offset);
            }
            send_packet(client_conn_fd, &list_res);
            printf("[*] List Server Res sent (size %u).\n", offset);
            break;
        }
        case PKT_SYNC_EVENT: // This packet type is defined but not used with specific logic
            printf("[*] PKT_SYNC_EVENT recebido de fd=%d, ignorando.\n", client_conn_fd);
            // No action needed as per original code. Could be used for heartbeats or explicit sync triggers.
            break;
        default:
            fprintf(stderr, "Tipo de pacote desconhecido (%d) recebido de fd=%d.\n", pkt->type, client_conn_fd);
            // Optionally send a NACK or error response
            break;
    }
}
