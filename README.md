Para compilar:
  (Dentro do dir build)
  cmake ..
  make

Para rodar server:
  (Dentro do dir build)
  ./server [id] [ip] [porta] [storage_root]

Para rodar cliente:
  (Dentro do dir build)
  ./client [user] [ip] [porta]

Para rodar frontend:
  (Dentro do dir build)
  ./frontend [ip] [porta]


Exemplo:

Server
./server 30 127.0.0.1 8003 ./storage_30

Client
./client john 127.0.0.1 9000

Frontend
./frontend 127.0.0.1 9000