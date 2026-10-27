10 int fi,fo,fw,c,k,n,s,q,t,u
20 dim char d(90):dim int v(255)
30 for c=48 to 57
31 v(c)=c-48
32 next
33 for c=65 to 70
34 v(c)=c-55
35 next
40 fi=fopen("AUX","r"):fo=fopen("AUX","w"):fw=fopen("RX.X","c"):fputc(35,fo)
100 c=fgetc(fi):if c<>58 then goto 100
110 n=0:s=0
120 c=fgetc(fi):if c<48 then goto 200
130 k=fgetc(fi) and 255:k=v(c)*16+v(k):d(n)=k:s=s+k:n=n+1:if n<90 then goto 120
140 goto 300
200 if (s and 255)<>0 or n<3 then goto 300
210 if d(n-2)<>n-3 then goto 300
220 if d(n-3)=((q-1) and 255) then fputc(75,fo):goto 100
230 if d(n-3)<>q then goto 300
240 if n=3 then goto 400
250 fwrite(d,n-3,fw):u=(u+s-d(n-3)-d(n-2)-d(n-1)) and 65535:t=t+n-3:q=(q+1) and 255:fputc(75,fo):goto 100
300 fputc(78,fo):goto 100
400 fclose(fw):fputc(75,fo):print "RX.X";t;"bytes"
410 fputc(83,fo):fputc(48+(u shr 12),fo):fputc(48+((u shr 8) and 15),fo):fputc(48+((u shr 4) and 15),fo):fputc(48+(u and 15),fo):fputc(13,fo)
420 exit(0)
