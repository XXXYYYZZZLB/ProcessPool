import socket  

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)  
server_address = ('172.21.183.36', 8888)  
sock.connect(server_address)  
  
message = 'hello.cgi\r\n'  
sock.sendall(message.encode(encoding='utf_8',errors='strict'))
 
data = sock.recv(1024)
print(data) 