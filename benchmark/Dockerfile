FROM ubuntu:24.04

# Install system dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    python3-requests \
    python3-numpy \
    python3-tabulate \
    python3-yaml \
    nginx-extras \
    libnginx-mod-http-lua \
    libnginx-mod-http-ndk \
    && rm -rf /var/lib/apt/lists/*

# Create working directory
WORKDIR /app

# Copy benchmark files
COPY benchmark.py requirements.txt ./

# Set environment variables
ENV PYTHONUNBUFFERED=1

# Default command
CMD ["python3", "benchmark.py", "--help"]