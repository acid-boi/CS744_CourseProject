# Key-Value Store

This project implements a simple key-value store with a C++ server, PostgreSQL database, and Redis cache.

## Prerequisites

- Docker
- Docker Compose

## How to Run

1. **Clone the repository:**

   ```bash
   git clone <repository-url>
   cd key-value-store
   ```

2. **Build and run the services using Docker Compose:**

   ```bash
   docker-compose up --build
   ```

   This command will build the Docker image for the C++ server and start the `server`, `postgres`, and `redis` services. The server will be running on `http://localhost:8080`.

## How to Use

You can interact with the server using a command-line tool such as `curl`.

### Save Data

To save a key-value pair, send a `POST` request to the `/put` endpoint:

```bash
curl -X POST -d "key=mykey&value=myvalue" http://localhost:8080/put
```

### Get Data

To retrieve the value for a key, send a `GET` request to the `/get` endpoint:

```bash
curl "http://localhost:8080/get?key=mykey"
```

### Delete Data

To delete a key-value pair, send a `DELETE` request to the `/delete` endpoint:

```bash
curl -X DELETE "http://localhost:8080/delete?key=mykey"
```
