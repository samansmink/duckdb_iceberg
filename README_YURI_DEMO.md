# README YURI
## To build 
- install c++ compiler, cmake, (optionally, but recommended: ninja, ccache)
- install vcpkg and configure, see https://github.com/duckdb/extension-template?tab=readme-ov-file#managing-dependencies
- run `make` or `make debug`

## To test this demo

Start the CLI:
```
./build/debug/duckdb
```

Create valid credentials. Following the blog post these are tmp sts credentials obtained with:
```shell
 aws sts assume-role --role-arn "arn:aws:iam::<redacted>:role/pyiceberg-etl-role" --role-session-name pyiceberg-etl-role
```

Then open duckdb and create the secret manually (for now, we have some work todo to make STS work)
```SQL
CREATE SECRET (
    TYPE S3,
    KEY_ID 'redacted',
    SECRET 'redacted',
    SESSION_TOKEN 'redacted',
    REGION 'us-east-1'
)
```

Now attach the S3 Tables catalog:
```SQL
ATTACH 'pyiceberg-blog-bucket' AS my_datalake (TYPE ICEBERG, CATALOG 'S3_TABLES_GLUE', ACCOUNT_ID 840140254803);
```

Fire away!

```SQL
SHOW ALL TABLES;
SELECT count(*) FROM my_datalake.myblognamespace.lineitem;
SELECT * FROM my_datalake.myblognamespace.lineitem LIMIT 10;
```







