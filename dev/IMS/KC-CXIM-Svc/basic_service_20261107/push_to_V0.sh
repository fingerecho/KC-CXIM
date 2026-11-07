v0_target_dir="/home/ubuntu/src/KC-CXIM/dev/IMS/KC-CXIM-Svc/basic_service_20261107"
my_ip="v0.fyping.cn"
my_user="ubuntu"
my_host="v0"
/d/Programs/rsync/bin/rsync.exe -avz --progress \
    -e '/d/Programs/rsync/bin/ssh.exe -i /cygdrive/c/Users/28328/.ssh/v0_id_rsa' \
    --exclude='build/' \
  . ${my_user}@${my_ip}:${v0_target_dir}
