const { performance } = require("node:perf_hooks");

function medir(nome, fn) {
    const inicio = performance.now();
    const resultado = fn();
    const tempo = (performance.now() - inicio) / 1000;
    console.log(`${nome}: ${tempo.toFixed(6)} s`);
    return tempo;
}

function loop() {
    let x = 0;
    for (let i = 0; i < 2000000; i++) x++;
    return x;
}

function aritmetica() {
    let x = 0;
    for (let i = 0; i < 1000000; i++)
        x = (i * 2 + 10) / 2 - 10;
    return x;
}

function condicoes() {
    let pares = 0, impares = 0;
    for (let i = 1; i <= 1000000; i++) {
        if (i % 2 === 0) pares++;
        else impares++;
    }
    return pares + impares;
}

function listas() {
    const lista = [1,2,3,4,5,6,7,8,9,10];
    let soma = 0;
    for (let i = 0; i < 200000; i++)
        for (const v of lista) soma += v;
    return soma;
}

function indices() {
    const lista = [1,2,3,4,5,6,7,8,9,10];
    let soma = 0;
    for (let i = 0; i < 3000000; i++)
        soma += lista[i % 10];
    return soma;
}

function strings() {
    let s = "";
    for (let i = 0; i < 200000; i++) s += "ab";
    return s.length;
}

function aninhado() {
    let x = 0;
    for (let i = 0; i < 1000; i++)
        for (let j = 0; j < 1000; j++) x++;
    return x;
}

let total = 0;
total += medir("1 Loop 2M", loop);
total += medir("2 Aritmetica 1M", aritmetica);
total += medir("3 Condicoes 1M", condicoes);
total += medir("4 Listas 200K", listas);
total += medir("5 Indices 3M", indices);
total += medir("6 Strings 200K", strings);
total += medir("7 Aninhado 1M", aninhado);

console.log(`TOTAL: ${total.toFixed(6)} s`);
