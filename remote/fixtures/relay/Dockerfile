# Minimal relay: sshd, user onion/relaypass, TCP forwarding on (Alpine ships it off).
# Build/run from the repo root:
#   docker build -t onion-fake-relay remote/fixtures/relay
#   docker network create onion-test
#   docker run -d --name onion-relay --network onion-test -p 2200:22 onion-fake-relay
FROM alpine:3.20
RUN apk add --no-cache openssh && ssh-keygen -A && adduser -D onion && echo 'onion:relaypass' | chpasswd \
 && sed -i 's/^AllowTcpForwarding no/AllowTcpForwarding yes/' /etc/ssh/sshd_config \
 && echo 'PasswordAuthentication yes' >> /etc/ssh/sshd_config
CMD ["/usr/sbin/sshd", "-D", "-e"]
