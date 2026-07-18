
## 1. SERVEUR — paquets système de base

```bash
sudo apt-get update
```

```bash
#Dans le dossier faasd, on lance ce script
sudo ./hack/install.sh    
```



### 2.2 Compiler  faasd (Go requis)
```bash
# depuis le dépôt du projet
cd benchmarks/micro/micro-bench3-keepalive-https-integration/faasd
make dist          # produit bin/faasd (x86_64) ET bin/faasd-arm64 (Pi)
```


> Prérequis : **Go 1.24**. La compilation est pure Go (`CGO_ENABLED=0`), donc cross-compilable
> depuis un x86_64 vers l'ARM sans toolchain spéciale.


```bash
# puis, SUR LE SERVEUR :
sudo install -m 755 /tmp/faasd /usr/local/bin/faasd
sudo systemctl restart faasd faasd-provider
```

Installer le client d'administration faas-cli (sur le serveur) :
```bash
curl -sSL https://cli.openfaas.com | sudo sh
```

Vérifier :
```bash
sudo systemctl status faasd faasd-provider containerd
/usr/local/bin/faasd version      
```



## 6. CLIENT — outils de charge et d'analyse

```bash
# 1) wrk2 (générateur open-loop)
sudo apt-get install -y build-essential libssl-dev git zlib1g-dev
git clone https://github.com/giltene/wrk2 ~/wrk2
make -C ~/wrk2                       


# 2) Python 3 + deps 
sudo apt-get install -y python3 python3-pip
pip3 install --break-system-packages matplotlib numpy pandas




# Gateway
docker buildx build --platform linux/arm64 \
  -f benchmarks/micro/micro-bench3-keepalive-https-integration/faas/gateway/Dockerfile \
  -t romerosdd/gateway-https:latest --push .



```bash
cd wolfssl && ./autogen.sh
./configure --enable-tls13 --enable-opensslextra --enable-atomicuser \
            --enable-sessionexport --enable-session-ticket --enable-keylog-export \
            --enable-aesgcm --enable-chacha --enable-hkdf --enable-aescbc \
            --enable-shared --enable-static
make -j$(nproc) && sudo make install && sudo ldconfig
```
