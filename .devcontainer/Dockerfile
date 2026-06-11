FROM mcr.microsoft.com/devcontainers/javascript-node:24-bookworm

ARG DEBIAN_FRONTEND=noninteractive

# System dependencies for ESP-IDF
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bison \
        ca-certificates \
        ccache \
        check \
        curl \
        flex \
        git \
        gperf \
        lcov \
        libbsd-dev \
        libffi-dev \
        libncurses-dev \
        libusb-1.0-0-dev \
        make \
        ninja-build \
        python3 \
        python3-pip \
        python3-venv \
        unzip \
        wget \
        xz-utils \
        zip \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/python python /usr/bin/python3 10

# Clone ESP-IDF v5.3
ENV IDF_PATH=/opt/esp/idf
ENV IDF_TOOLS_PATH=/opt/esp

RUN git clone --recursive \
        --depth=1 --shallow-submodules \
        -b v5.3 \
        https://github.com/espressif/esp-idf.git \
        ${IDF_PATH} \
    && git config --system --add safe.directory ${IDF_PATH}

# Install toolchains for ESP32-S3
RUN update-ca-certificates --fresh \
    && ${IDF_PATH}/tools/idf_tools.py --non-interactive install required --targets=esp32s3 \
    && ${IDF_PATH}/tools/idf_tools.py --non-interactive install cmake \
    && ${IDF_PATH}/tools/idf_tools.py --non-interactive install-python-env \
    && rm -rf ${IDF_TOOLS_PATH}/dist

ENV IDF_PYTHON_CHECK_CONSTRAINTS=no
ENV IDF_CCACHE_ENABLE=1

# Capture the fully-resolved PATH after sourcing export.sh and persist it
# so idf.py works in every shell without manual sourcing
RUN IDF_EXPORT_PATH=$(bash -c '. ${IDF_PATH}/export.sh > /dev/null 2>&1 && echo $PATH') \
    && IDF_PYTHON_ENV=$(bash -c '. ${IDF_PATH}/export.sh > /dev/null 2>&1 && echo $IDF_PYTHON_ENV_PATH') \
    && { \
        echo ""; \
        echo "# ESP-IDF environment"; \
        echo "export IDF_PATH=\"${IDF_PATH}\""; \
        echo "export IDF_TOOLS_PATH=\"${IDF_TOOLS_PATH}\""; \
        echo "export IDF_PYTHON_CHECK_CONSTRAINTS=no"; \
        echo "export IDF_CCACHE_ENABLE=1"; \
        echo "export IDF_PYTHON_ENV_PATH=\"${IDF_PYTHON_ENV}\""; \
        echo "export PATH=\"${IDF_EXPORT_PATH}\""; \
    } | tee -a /etc/bash.bashrc > /etc/profile.d/esp-idf.sh \
    && chmod +x /etc/profile.d/esp-idf.sh \
    && echo "PATH=\"${IDF_EXPORT_PATH}\"" >> /etc/environment

# Allow node user to use ESP-IDF tools
RUN chmod -R a+rX /opt/esp
