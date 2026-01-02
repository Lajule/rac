FROM gcc:trixie AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libuv1-dev \
    libllhttp-dev \
    libpq-dev

WORKDIR /src

COPY . /src

RUN gcc -Wall -o rac main.c cJSON.c -luv -lpq -lllhttp

FROM debian:trixie

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libuv1-dev \
    libllhttp-dev \
    libpq-dev

COPY --from=builder /src/rac /app/rac

CMD ["/app/rac"]
