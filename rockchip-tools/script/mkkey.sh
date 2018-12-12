#!/bin/sh
AUTH='/C=US/ST=California/L=Mountain View/O=Android/OU=Android/CN=Android/emailAddress=android@android.com'
if [ "$1" = "" ]; then
        echo "Create a test certificate key."
        echo "Usage: $0 NAME"
        echo "Will generate NAME.pk8 and NAME.x509.pem"
        echo "  $AUTH"
        exit
fi

openssl genrsa -3 -out $1.pem 2048

openssl req -new -x509 -key $1.pem -out $1.x509.pem -days 10000 \
    -subj "$AUTH"

echo "Please enter the password for $1 key:"
openssl pkcs8 -in $1.pem -topk8 -outform DER -out $1.pk8 -passout stdin
