# ModSecurity-envoy
The ModSecurity-Envoy is Envoy version compiled with HTTP filter (can be opt-in/out) running ModSecurity (V3).
In other words you can run and configure WAF (ModSecurity) rules on HTTP Traffic that flows through envoy.

The most common use case is for ModSecurity-Envoy is to apply WAF on East-West traffic inside kubernetes deployments.
As Envoy is the de-facto standard proxy in kubernetes deployments and is usually deployed in every pod you can deploy
this Envoy version and Enable ModSecurity-Envoy Filter on all pods or on the most important ones.

Some of the ideas behind the project are described in this [blog](https://github.com/octarinesec/ModSecurity-envoy)

## Compilation

### Dependencies

ModSecurity-Envoy depends on Envoy (as a git submodule) and ModSecurity (a sibling directory).
The [modsecurity](./modsecurity) directory contains two symbolic links to the sibling directory.

```bash
git clone git@github.com:octarinesec/ModSecurity-envoy.git
git clone git@github.com:SpiderLabs/ModSecurity.git

cd ModSecurity-envoy
git submodule update --init
```

The directory structure should be as follows:
```
.
+-- ModSecurity-envoy
|  +-- modsecurity
|  |  +-- include -> ../../ModSecurity/headers
|  |  +-- libmodsecurity.a -> ../../ModSecurity/src/.libs/libmodsecurity.a
+-- ModSecurity
```

For more details on how to compile ModSecurity read ModSecurity's [documentation](https://github.com/SpiderLabs/ModSecurity#compilation).

### Compiling on host

You can compile ModSecurity-Envoy on host, the same as you would compile Envoy.
However, you will need these additional dependencies:

```bash
sudo apt-get install -y libtool cmake realpath clang-format-5.0 automake 
sudo apt-get install -y g++ flex bison curl doxygen libyajl-dev libgeoip-dev libtool dh-autoreconf libcurl4-gnutls-dev libxml2 libpcre++-dev libxml2-dev
```

To build run
```bash
./bin/build-local.sh
```

For more information on envoy's building system read Envoy's [documentation](https://github.com/envoyproxy/envoy).

### Using the docker images

You can build docker images for envoy-build and envoy by running:

```bash
./bin/build_envoy_modsec.sh
```

## Configuration

ModSecurity-Envoy Filter accept the configuration defined in [http_filter.proto](./http-filter-modsecurity/http_filter.proto)

You will need to modify the Envoy config file to add the filter to the filter chain for a particular HTTP route configuration. 
See the examples in [conf](conf).

Note: By adding metadata to specific routes, you can have granular control to disable the filter:
```yaml
metadata:
  filter_metadata:
    envoy.filters.http.modsecurity:
      # To only disable requests / responses processing
      # disable_request: true
      # disable_response: true
      # Or, as a shorthand, use disable to disable both
      disable: true
      # set to true to disable default json audit log on envoy log output
      no_audit_log: false
```

The configuration for the filter is provided under the http_filters:
```yaml
        http_filters:
        # before envoy.router because order matters!
        - name: envoy.filters.http.modsecurity
          typed_config:
            "@type": type.googleapis.com/modsecurity.Decoder
            # ModSecurity rules can either be provided by a list of path
            rules_path: [/etc/modsecurity.conf]
            # Additionally you can provide a list of inline rules (will be loaded after processing the rules_path, if provided)
            rules_inline: 
            - |
              # ModSecurity rules
              # ...
            # List of remotes url to retrieve rules
            # Key is given in http header `ModSec-key`
            remotes:
            - key: a-key
              url: https://url.com/my-rules
            # If set to true, if no errors occured during remote download, those rules will overwrite all rules.
            remotes_overwrite_on_success: true
        - name: envoy.router
          config: {}
```

## OWASP ModSecurity Core Rule Set (CRS)

CRS is a set of generic attack
detection rules for use with ModSecurity and aims to protect web applications
from wide range of attacks. For more information check out [https://modsecurity.org/crs/](https://modsecurity.org/crs/)

Download and extract the latest rules to the directory.

```bash
wget https://github.com/SpiderLabs/owasp-modsecurity-crs/archive/v3.1.1.tar.gz
tar xvzf v3.1.1.tar.gz
```

The configuration examples include the relevant OWASP rules.
See [./conf/modsecurity.conf](./conf/modsecuirty.conf) and [./conf/lds.yaml](./conf/lds.yaml) for usage example.

## Testing

TODO

## How it works

First let's run an echo server that we will use as our upstream

```bash
docker run -p 5555:80 kennethreitz/httpbin
```

Now let's run the envoy

```bash
sudo ./bazel-bin/envoy-static -c conf/envoy-modsecurity-example-lds.yaml -l info
```

Make our first request
```bash
curl -X GET "http://127.0.0.1:8585/get" -H "accept: application/json"
```

Let's download Nikto which is the most popular Open Source web server scanner

```bash
wget https://github.com/sullo/nikto/archive/master.zip
unzip master.zip
perl nikto-master/program/nikto.pl -h localhost:5555
```

Now we can `cat /var/log/modsec_audit.log` and see all detected attacks which in production
can be piped to a SIEM of your choice or any other centralized log.

Let's try and add our own RULE as each WAF are designed to be configurable to protect
different web applications.

Make sure the following line is in `modsecurity-example.conf` or in the configuration under rules_inline.

`SecRule ARGS:param1 "test" "id:1,deny,msg:'this',msg:'is',msg:'a',msg:'test'"`

This line will detect any url with argument ?param1=test param.

reload Envoy's configuration and execute the following command
`curl -X GET "http://127.0.0.1:8585/get?param1=test" -H "accept: application/json"`

check the logs via `tail -f` and you will see the following output

```bash
ModSecurity: Warning. Matched "Operator `Rx' with parameter `test' against variable `ARGS:param1' (Value: `test' ) [file "crs-setup.conf"] [line "7"] [id "1"] [rev ""] [msg "test"] [data ""] [severity "0"] [ver ""] [maturity "0"] [accuracy "0"] [hostname ""] [uri "/"] [unique_id "152991475598.002681"] [ref "o0,4v13,4"]
```