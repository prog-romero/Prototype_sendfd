#!/bin/sh
# start.sh — lancé par le watchdog comme fprocess (UN SEUL token, pas de
# guillemets). Le watchdog of-watchdog découpe fprocess par ESPACES sans
# respecter les quotes ; impossible donc de lui passer `sh -c '...'`. On délègue
# le vrai lancement (avec expansion de ${GUNICORN_WORKERS}) à ce script.
exec gunicorn \
  --workers "${GUNICORN_WORKERS:-4}" \
  --bind 127.0.0.1:8085 \
  --timeout 120 \
  server:app
