FROM debian:trixie AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    libuv1-dev \
    libllhttp-dev \
    libpq-dev

WORKDIR /app

COPY . /app

RUN gcc -Wall -o rac main.c cJSON.c -luv -lpq -lllhttp

CMD ["/app/rac"]
