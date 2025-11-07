for i in {1..10}; do
  key="k$i"
  val="v$i"
  echo "Testing key=$key val=$val"
  curl -s -X POST "http://127.0.0.1:8080/put" -d "key=$key" -d "value=$val"
  echo
  curl -s "http://127.0.0.1:8080/get?key=$key"
  curl -s -X DELETE "http://127.0.0.1:8080/delete" -d "key=$key"
  echo -e "\n---"
done

