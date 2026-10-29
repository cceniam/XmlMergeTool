# Synthetic test data

`test.xml` contains only synthetic test data from
`cpp/tests/fixtures/schema3.xml`. Its closing `</robot>` tag is deliberately
omitted so the native report CLI must reject it, while desktop recovery and
Python/C++ differential tests use the same repaired input.

The schema 3 fixture was generated from `cpp/tests/fixtures/features.robot`.
Its suite source is a repository-relative path. It contains no device access,
customer logs, credentials, or workstation paths.
