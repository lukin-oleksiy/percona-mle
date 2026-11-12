# Percona Server with component_mle (JavaScript, Python + numpy, Java) on
# Ubuntu 26.04. Built by build-image.sh from a release build installed into
# stage/ (cmake --install) and a GraalVM JDK 25 in graalvm/ (POC: the image is
# made from a local build; a reproducible multi-stage build comes later).
FROM ubuntu:26.04

# The libraries mysqld, its plugins and the MLE binaries need (from ldd of the
# installed tree on Ubuntu 26.04).
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libaio1t64 libatomic1 libcurl4t64 libfido2-1 libgssapi-krb5-2 libkrb5-3 \
      libldap2 libnuma1 libsasl2-2 libssl3t64 libtinfo6 libtirpc3t64 libzstd1 \
      zlib1g tini ca-certificates \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# mysqld runs as mysql; MLE workers run as mysqld's user (19 §2).
RUN groupadd --system mysql && useradd --system --gid mysql --home /var/lib/mysql \
      --shell /usr/sbin/nologin mysql

COPY stage/ /usr/local/mysql/
COPY graalvm/ /opt/graalvm/
COPY my.cnf /etc/mysql/my.cnf
COPY mle-entrypoint.sh /usr/local/bin/mle-entrypoint.sh

ENV PATH=/usr/local/mysql/bin:$PATH
RUN mkdir -p /var/lib/mysql /var/run/mysqld /var/lib/mysql-mle-packages \
 && chown mysql:mysql /var/lib/mysql /var/run/mysqld \
 && chmod 0755 /var/lib/mysql-mle-packages \
 && mysqld --version

VOLUME ["/var/lib/mysql", "/var/lib/mysql-mle-packages"]
EXPOSE 3306
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/mle-entrypoint.sh"]
CMD ["mysqld"]
