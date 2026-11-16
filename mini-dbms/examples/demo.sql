-- Phase 1 데모: ./build/mdb examples/demo.sql
-- (같은 파일을 두 번 실행하면 데이터가 한 번 더 추가됩니다)

INSERT INTO users VALUES ('민수', 25);
INSERT INTO users (age, name) VALUES (30, '영희'), (22, 'O''Brien, Jr');   -- 컬럼 순서 바꾸기 + 여러 행
insert into users values ('kim, lee', 41);                                 -- 소문자 키워드, 문자열 안의 쉼표

SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;          -- B+ 트리 INDEX LOOKUP
SELECT * FROM users WHERE id >= 2 LIMIT 2;         -- B+ 트리 INDEX RANGE SCAN
SELECT id, name FROM users WHERE age > 24;         -- FULL SCAN (선형 탐색)

INSERT INTO products VALUES ('keyboard', 45000), ('mouse', 19000);
SELECT title FROM products WHERE price < 20000;

-- 아래는 일부러 넣은 에러들: 에러가 나도 다음 문장은 계속 실행됩니다
SELECT * FROM orders;                              -- 없는 테이블        (Executor)
INSERT INTO users VALUES ('no age');               -- 값 개수 불일치      (Executor)
INSERT INTO users VALUES ('x', 'twenty');          -- 타입 불일치        (Executor)
SELECT * FROM users WHERE name = 'unterminated;    -- 닫히지 않은 문자열  (Tokenizer)
