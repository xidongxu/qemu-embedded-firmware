set pagination off
set confirm off
target remote :1234
continue &
shell powershell -NoProfile -Command "Start-Sleep -Seconds 20"
interrupt
bt 12
info registers r14 r15
quit
