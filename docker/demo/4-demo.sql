-- Demo 4: errors at CREATE and at run time (as the application account, after
-- docker/run.sh root --force < 4-setup.sql).

-- Compile time. CREATE checks the code: a Python syntax error names the line, on either Python worker.
CREATE FUNCTION py_bad(a INT) RETURNS INT LANGUAGE PYTHON AS $$
    b = a +* 2
    return b
$$;

CREATE FUNCTION cpy_bad(a INT) RETURNS INT LANGUAGE CPYTHON AS $$
    b = a +* 2
    return b
$$;

-- Java is really compiled: a type error is found before anything runs.
CREATE FUNCTION java_bad(a INT) RETURNS INT LANGUAGE JAVA AS $$
    String s = a;
    return s.length();
$$;

-- JavaScript shows the place, on GraalJS and on V8.
CREATE FUNCTION gjs_bad(a INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$
    return a +* 2
$$;

CREATE FUNCTION v8_bad(a INT) RETURNS INT LANGUAGE JS AS $$
    return a +* 2
$$;

-- Python cannot know every name at CREATE, but it warns about names that are never defined.
CREATE FUNCTION py_typo(a INT) RETURNS INT LANGUAGE PYTHON AS $$
    return a + offset
$$;

SHOW WARNINGS;

-- Run time. This function divides in its helper, on line 4.
CREATE FUNCTION py_share(total INT, parts INT) RETURNS INT LANGUAGE PYTHON AS $$
    def per_part(total, parts):
        print('dividing', total, 'by', parts)
        return total // parts
    return per_part(total, parts)
$$;

SELECT py_share(10, 2);

-- What the routine printed came back with the statement, as warnings.
SHOW WARNINGS;

-- With parts = 0 the call fails: the error names the routine, the line and the function.
SELECT py_share(10, 0);

-- The traceback, with the source lines, comes back as warnings too.
SHOW WARNINGS;

-- The same body on the CPython worker gives the same answer.
CREATE FUNCTION cpy_share(total INT, parts INT) RETURNS INT LANGUAGE CPYTHON AS $$
    def per_part(total, parts):
        print('dividing', total, 'by', parts)
        return total // parts
    return per_part(total, parts)
$$;

SELECT cpy_share(10, 0);

-- JavaScript on V8, and Java.
CREATE FUNCTION v8_share(total INT, parts INT) RETURNS INT LANGUAGE JS AS $$
    function per_part(total, parts) {
        if (parts === 0) throw new RangeError('no parts')
        return Math.floor(total / parts)
    }
    return per_part(total, parts)
$$;

SELECT v8_share(10, 0);

CREATE FUNCTION java_share(total INT, parts INT) RETURNS INT LANGUAGE JAVA AS $$
    int per = total / parts;
    return per;
$$;

SELECT java_share(10, 0);

SHOW WARNINGS;

-- Trying a snippet needs no CREATE.
SELECT mle_eval('python', 'import numpy; numpy.arange(5) ** 2') AS python,
       mle_eval('javascript', '[1, 2, 3].map(x => x * x)') AS javascript;
