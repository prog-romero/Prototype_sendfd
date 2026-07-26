#!/bin/sh
# start.sh — lancé par le watchdog comme fprocess (UN SEUL token : le watchdog
# découpe fprocess par ESPACES sans respecter les quotes, donc pas de sh -c).
# Délègue le lancement de gunicorn (avec ${GUNICORN_WORKERS}) à ce script.
exec gunicorn \
  --workers "${GUNICORN_WORKERS:-4}" \
  --bind 127.0.0.1:8085 \
  --timeout 120 \
  server:app
