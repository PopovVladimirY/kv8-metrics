# Kafka Custom Configuration

This directory is bind-mounted into the container at `/opt/kafka-custom`.
Place any custom Kafka property overrides or JAAS config files here.

## Quick Start

```bash
cd docker
docker compose up -d
```

## Connection Details

Two listeners are available. Use the one that matches where the client runs.

### Same machine (localhost)

| Setting | Value |
|---|---|
| Bootstrap servers | `localhost:19092` |
| Security protocol | `SASL_PLAINTEXT` |
| SASL mechanism | `PLAIN` |
| Username | `kv8producer` |
| Password | `kv8secret` |

### Remote host on local network (LAN)

| Setting | Value |
|---|---|
| Bootstrap servers | `<host-lan-ip>:19093` |
| Security protocol | `SASL_PLAINTEXT` |
| SASL mechanism | `PLAIN` |
| Username | `kv8producer` |
| Password | `kv8secret` |

To enable LAN access, set `KAFKA_HOST_IP` in `docker/.env` to the host
machine's LAN IP address before starting (or restarting) the container:

```ini
# docker/.env
KAFKA_HOST_IP=192.168.1.42
```

Then restart the broker:

```bash
docker compose restart kafka
```

Remote clients connect to `<KAFKA_HOST_IP>:19093` using the same SASL/PLAIN
credentials as for the local listener.

## Kv8 Client Usage

Local (same machine):
```
kv8feeder.exe /KV8.brokers=localhost:19092 /KV8.user=kv8producer /KV8.pass=kv8secret
```

Remote (LAN), substitute `192.168.1.42` with your host's actual LAN IP:
```
kv8feeder.exe /KV8.brokers=192.168.1.42:19093 /KV8.user=kv8producer /KV8.pass=kv8secret
```

## Credentials

Credentials are defined in `docker/.env` and injected into the container as
environment variables. The broker's SASL/PLAIN JAAS configuration is written
at container startup from those variables by the `entrypoint` in
`docker-compose.yml`. To change credentials, edit `.env` and restart:

```bash
docker compose restart kafka
```

To add a second client user, add a `KAFKA_CLIENT_USERS_2` / `KAFKA_CLIENT_PASSWORDS_2`
pair to `.env` and extend the entrypoint heredoc in `docker-compose.yml` with
an extra `user_<name>="<password>"` line.

`kafka_server_jaas.conf` in this directory is a reference copy reflecting the
default credential layout; it is not mounted into the container.

## Volume Mapping

| Host / Named Volume | Container Path | Purpose |
|---|---|---|
| `kafka-data` (Docker volume) | `/tmp/kraft-combined-logs` | Broker log segments (topic data) |

## Stopping

```bash
docker compose down       # stop, keep data
docker compose down -v    # stop and delete all data
```
