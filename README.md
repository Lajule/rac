# rac

🚀 High-Performance REST API in C

> **Bye Golang, Ciao Rust, Thank you AI, Welcome back C!**

A REST API built entirely in C with: async I/O, connection pooling, and thread-based request handling.

## ✨ Features

- ⚡ **Asynchronous I/O** - Built on libuv event loop for maximum performance
- 📊 **PostgreSQL Pool** - Thread-safe connection pooling
- 🧵 **Thread Pool** - Multi-threaded request processing
- 🔄 **HTTP Parser** - Modern llhttp parser for efficient request handling
- 📦 **JSON Support** - Request/response serialization with cJSON

## 📋 Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y \
    libuv1-dev \
    libllhttp-dev \
    libpq-dev \
    build-essential
```

## 🙏 Acknowledgments

- [libuv](https://libuv.org/) - Cross-platform async I/O
- [llhttp](https://github.com/nodejs/llhttp) - Fast HTTP parser
- [cJSON](https://github.com/DaveGamble/cJSON) - Lightweight JSON parser
- [PostgreSQL](https://www.postgresql.org/) - Powerful database
- [libpq](https://www.postgresql.org/docs/current/libpq.html) - PostgreSQL C API

---

**Built with ❤️ and C** - Because sometimes, less is more.