Para compilar:
  (Dentro do dir build)
  cmake ..
  make

Para rodar server:
  (Dentro do dir build)
  ./server [ip] [porta]

Para rodar cliente:
  (Dentro do dir build)
  ./client [user] [ip] [porta]


Exemplo:

Server
../../dropbox/build/server 127.0.0.1 8003 30
../../dropbox/build/server 127.0.0.1 8002 20
../../dropbox/build/server 127.0.0.1 8001 10

Client
../../dropbox/build/client username 127.0.0.1 8003