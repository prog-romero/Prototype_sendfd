// Hôte du template — copie FIDÈLE de l'OpenFaaS `node10-express-service`.
//
// C'est exactement le serveur que BeFaaS utilise pour exposer une fonction sur
// OpenFaaS : il crée une app Express, appelle handler({app}) UNE fois au
// démarrage (ce qui branche la vraie fonction BeFaaS sur les routes), puis
// écoute. Ce fichier ne contient AUCUNE logique métier : il ne fait qu'héberger
// la fonction (functions/<nom>/index.js) via ./function/handler.js.
//
// Le watchdog (vanilla ou full-proxy) reverse-proxy chaque requête entrante
// vers ce serveur sur 127.0.0.1:${http_port}.

'use strict'

const express = require('express')
const app = express()
const handler = require('./function/handler')

async function init () {
  await handler({ app: app })

  // Port d'écoute : aligné sur http_upstream_url du watchdog (8085 par défaut).
  const port = process.env.http_port || 8085
  app.disable('x-powered-by')

  app.listen(port, () => {
    console.log(`[befaas-host] ${process.env.BEFAAS_FN_NAME || 'fn'} listening on :${port}`)
  })
}

init()
