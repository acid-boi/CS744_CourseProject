#!/bin/bash

while [ ! -e /dev/sdb1 ]; do
  read -p "Please insert the pendrive and press Enter to continue..."
done

docker-compose up -d

echo "server is running"
exit 0
