%read raw sensor data
M = csvread('I:\0001.DAT.csv',2);
[length,line]=size(M);
%plot curve12
x_aixs=1:length;
figure(1)
%plot(x_aixs,M(:,16),'r',x_aixs,M(:,27),'b')
plot(x_aixs,M(:,16),'r',x_aixs,M(:,27),'b')
figure(2)
%plot(x_aixs,M(:,17),'r',x_aixs,M(:,28),'b')
plot(x_aixs,M(:,17),'r',x_aixs,M(:,28),'b')
figure(3)%plot 3D postion
plot3(M(:,24),M(:,25),M(:,26),'o')
figure(4)%ned speed estimate
plot(x_aixs,M(:,27),'b',x_aixs,M(:,28),'r')
