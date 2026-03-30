#!/bin/bash
set -e

# Nettoyage si le dossier existe déjà
if [ -d "wolfssl" ]; then
    rm -rf wolfssl
fi

echo "--- Clonage de wolfSSL ---"
git clone https://github.com/wolfSSL/wolfssl.git
cd wolfssl

echo "--- Génération du script de configuration ---"
./autogen.sh

echo "--- Configuration avec Export de Session ---"
# Note : sessionexport (sans tiret après session) est le flag correct
# Nous ajoutons --enable-sni pour votre routage par nom de fonction
./configure --enable-sessionexport \
            --enable-sni \
            --enable-tls13 \
            --enable-all

echo "--- Compilation ---"
make -j$(nproc)

echo "--- Installation ---"
sudo make install
sudo ldconfig

echo "--- Vérification des fonctions ---"
grep -r "wolfSSL_tls_export" ./wolfssl/ssl.h || echo "Attention: Fonction non trouvée dans ssl.h"

#nm -D /usr/local/lib/libwolfssl.so | grep wolfSSL_tls_export
#openssl req -x509 -newkey rsa:2048 -keyout server-key.pem -out server-cert.pem -days 365 -nodes -subj "/CN=localhost"