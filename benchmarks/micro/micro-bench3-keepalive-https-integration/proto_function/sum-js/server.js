'use strict'
// server.js — serveur JS (Express) inspiré du mode BeFaaS (node10-express-service).
// C'est LE serveur qui accepte et parse toutes les requêtes ; le watchdog
// (vanilla ou full-proxy) le reverse-proxy sur 127.0.0.1:8085. Il ne fait qu'une
// chose : la somme de deux nombres — volontairement trivial pour que le CPU
// mesuré soit celui du RUNTIME (Node/Express), pas du calcul.
//
// Deux formats de corps acceptés (compat client d'éval) :
//   - JSON  : {"a": 3, "b": 4}
//   - texte : "3 4"  (mêmes fichiers d'entrée que la fonction C : sumprod.txt…)

const express = require('express')
const app = express()
app.disable('x-powered-by')

// Lit TOUT corps comme texte brut (n'importe quel Content-Type) ; on tente
// ensuite JSON, sinon on extrait les deux premiers entiers.
app.use(express.text({ type: '*/*', limit: '32mb' }))

function parseAB (raw) {
  try {
    const j = JSON.parse(raw)
    if (j && typeof j === 'object' && ('a' in j || 'b' in j)) {
      return [Number(j.a) || 0, Number(j.b) || 0]
    }
  } catch (_e) { /* pas du JSON -> parsing texte ci-dessous */ }
  const nums = String(raw || '').match(/-?\d+/g) || []
  return [parseInt(nums[0] || '0', 10), parseInt(nums[1] || '0', 10)]
}

// Route attrape-tout : marche que faasd ait réécrit l'URL en `/` (vanilla) ou
// que le gateway ait rejoué `/function/<nom>` (proto).
app.all('*', (req, res) => {
  const [a, b] = parseAB(req.body)
  res.json({ status: 'success', result: a + b })
})

const port = process.env.http_port || 8085
app.listen(port, '127.0.0.1', () => {
  console.log(`[sum-js] listening on 127.0.0.1:${port}`)
})
