ROOT=$(dirname $(readlink -f $0))

mkdir -p $ROOT/code

# 下载预制引导盘2.7G
if [ ! -d $ROOT/code/dqib_amd64-pc ]; then
    wget https://bit.ly/cemu-dqib -O $ROOT/code/dqib.tar.xz
    tar -xJf $ROOT/code/dqib.tar.xz -C $ROOT/code
fi
