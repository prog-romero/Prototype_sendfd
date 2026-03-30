# 🥔 Hot Potato TLS Migration (wolfSSL)

Ce projet implémente une architecture de **FaaS (Function as a Service)** sécurisée avec une optimisation de migration TLS nommée "Hot Potato". 

L'objectif est de permettre à un client de maintenir une seule connexion HTTPS (`Keep-Alive`) tout en faisant traiter ses requêtes par différents micro-services (Workers) sans jamais ré-établir de handshake TLS complet entre les changements de service.

## 🚀 Fonctionnement : Le concept "Hot Potato"

1.  **Gateway** : Reçoit la connexion initiale, effectue le Handshake TLS, puis passe "la patate chaude" (le descripteur de fichier + l'état cryptographique TLS) au Worker concerné via des sockets Unix (`SCM_RIGHTS`).
2.  **Workers** : Importent la session TLS, traitent la requête et répondent au client.
3.  **Keep-Alive Intelligent** : Si la requête suivante est pour le même Worker, il la traite directement. Si elle change (ex: passage de SUM à PRODUCT), le Worker renvoie la connexion à la Gateway qui la redirige vers le nouveau Worker.

---

## 🛠 Pré-requis

- **Linux** (nécessaire pour `SCM_RIGHTS` et les sockets Unix).
- **wolfSSL** installé dans `/usr/local/` (avec l'option `--enable-sessionexport`).
- **GCC** et **Make**.

Si wolfSSL n'est pas installé sur votre machine, vous pouvez utiliser le script fourni :
```bash
chmod +x setup_worlfssl.sh
sudo ./setup_worlfssl.sh
```

---

## 📦 Compilation

Utilisez le Makefile pour compiler les trois composants (Gateway, Worker SUM, Worker PRODUCT) :

```bash
make clean && make
```

---

## 🏃 Comment exécuter

Il est recommandé d'ouvrir **4 terminaux** pour observer les logs en temps réel.

1.  **Terminal 1 (Worker SUM)** :
    ```bash
    ./worker_sum
    ```
2.  **Terminal 2 (Worker PRODUCT)** :
    ```bash
    ./worker_product
    ```
3.  **Terminal 3 (Gateway)** :
    ```bash
    ./gateway
    ```

---

## 🧪 Comment tester

### Test de migration dynamique (Keep-Alive)
Utilisez cette commande `curl` qui enchaîne plusieurs calculs différents sur la **même connexion** (grâce au Keep-Alive) :

```bash
curl -k "https://localhost:8443/sum?a=10&b=20" \
        "https://localhost:8443/product?a=5&b=6" \
        "https://localhost:8443/sum?a=1&b=2"
```

### Ce que vous devez observer :
- **Dans le Gateway** : Vous verrez un seul log `Handshake TLS réussi` au début. Ensuite, vous verrez des logs `Recu retour Keep-Alive` uniquement quand la route change.
- **Dans les Workers** : Le worker garde le client tant que les requêtes sont pour lui. Dès qu'un changement est détecté (ex: SUM voit passer PRODUCT), il renvoie la connexion à la Gateway.
- **Dans curl** : Les réponses arrivent instantanément sur le même flux SSL/TLS.

---

## 📂 Structure technique

- `gateway.c` : Orchestrateur. Gère le port 8443 et le retour des connexions via `/tmp/faas_back.sock`.
- `worker_sum.c` & `worker_product.c` : Micro-services de calcul. Ils possèdent une boucle intelligente qui décide s'ils gardent ou migrent la connexion.
- `common.h` : Structure `migration_msg_t` contenant le blob TLS (session exportée) et les paramètres.

---
*Projet réalisé dans le cadre du Master 2 ACS - SUPAERO ISAE.*
