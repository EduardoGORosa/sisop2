# SERVER
docker build -f Dockerfile.server -t dropbox-server .

sudo docker run -d --name dropbox-server \
  --network host \
  -v "$(pwd)/storage:/app/storage" \
  dropbox-server


#CLIENT
docker build -f Dockerfile.client -t dropbox-client .

docker run -it --rm --name dropbox-client \
            --network host \
            -v "$(pwd)/client1_data:/app/sync_dir" \
            dropbox-client joao localhost 12345
