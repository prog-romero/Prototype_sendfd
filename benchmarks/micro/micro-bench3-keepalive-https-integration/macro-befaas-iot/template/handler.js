// handler.js — équivalent du handler.js que BeFaaS génère dans son build.sh :
//
//   module.exports = async (config) => {
//     config.app.all('/*', require('./index.js').openfaasHandler)
//   }
//
// Il branche le VRAI handler de la fonction BeFaaS (l'app Koa exportée par
// @befaas/lib via .openfaasHandler) sur toutes les routes de l'app Express.
//
// SEULE addition par rapport à BeFaaS : la normalisation du chemin.
//
//   Pourquoi ? Pour OpenFaaS, @befaas/lib n'utilise PAS de préfixe de route
//   (helper.prefix() renvoie null) : le routeur Koa attend donc exactement
//   `POST /call` (fonctions rpc) ou `POST /` (objectrecognition).
//
//   - En mode VANILLA (gateway faasd standard), le préfixe `/function/<nom>`
//     est déjà retiré : le serveur reçoit `/call`. -> OK tel quel.
//   - En mode PROTOTYPE (migration sendfd), le gateway rejoue les octets BRUTS
//     de la requête : le serveur reçoit `/function/<nom>/call`. -> il faut
//     retirer le préfixe ici pour que le routeur Koa matche, SANS toucher au
//     watchdog.
//
//   Cette normalisation est idempotente : si le préfixe est absent (vanilla),
//   l'URL est laissée intacte. Les deux modes aboutissent donc à `/call`.

'use strict'

module.exports = async (config) => {
  // require résolu vers ./function/index.js (la fonction BeFaaS, deps installées
  // dans ./function/node_modules). C'est le code BeFaaS NON MODIFIÉ.
  const koaHandler = require('./index.js').openfaasHandler

  const fnName = process.env.BEFAAS_FN_NAME || ''

  const normalizePath = (url) => {
    if (!fnName) return url
    const prefix = '/function/' + fnName
    if (url === prefix) return '/'
    if (url.startsWith(prefix + '/')) return url.slice(prefix.length)
    if (url.startsWith(prefix + '?')) return '/' + url.slice(prefix.length)
    return url
  }

  config.app.all('/*', (req, res) => {
    req.url = normalizePath(req.url)
    koaHandler(req, res)
  })
}
