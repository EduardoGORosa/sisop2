# se ainda não tiver feito:
docker build -f Dockerfile.server -t dropbox-server .

# roda em background, mapeando a porta 12345
sudo docker run -it --rm --name dropbox-client \
            --network host \
            -v "$(pwd)/client1_data:/app/sync_dir" \
            dropbox-client joao localhost 12345
# primeiro construa a imagem cliente, se ainda não fez:
docker build -f Dockerfile.client -t dropbox-client .

docker run -it --rm --name dropbox-client \
            --network host \
            -v "$(pwd)/client1_data:/app/sync_dir" \
            dropbox-client joao localhost 12345
