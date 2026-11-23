import time

def medir(nome, fn):
    inicio = time.perf_counter()
    resultado = fn()
    tempo = time.perf_counter() - inicio
    print(f"{nome}: {tempo:.6f} s")
    return tempo

# 1. Loop e incremento — 2 milhões
def loop():
    x = 0
    for i in range(2_000_000):
        x += 1
    return x

# 2. Operações aritméticas — 1 milhão
def aritmetica():
    x = 0
    for i in range(1_000_000):
        x = (i * 2 + 10) / 2 - 10
    return x

# 3. Condições — 1 milhão
def condicoes():
    pares = 0
    impares = 0
    for i in range(1, 1_000_001):
        if i % 2 == 0:
            pares += 1
        else:
            impares += 1
    return pares + impares

# 4. Listas — 200 mil iterações externas
def listas():
    lista = [1,2,3,4,5,6,7,8,9,10]
    soma = 0
    for i in range(200_000):
        for v in lista:
            soma += v
    return soma

# 5. Acesso por índice — 3 milhões
def indices():
    lista = [1,2,3,4,5,6,7,8,9,10]
    soma = 0
    for i in range(3_000_000):
        soma += lista[i % 10]
    return soma

# 6. Concatenação — 200 mil
def strings():
    s = ""
    for i in range(200_000):
        s += "ab"
    return len(s)

# 7. Loop aninhado — 1 milhão
def aninhado():
    x = 0
    for i in range(1000):
        for j in range(1000):
            x += 1
    return x

# EXECUÇÃO — somente depois de definir todas as funções
total = 0

total += medir("1 Loop 2M", loop)
total += medir("2 Aritmetica 1M", aritmetica)
total += medir("3 Condicoes 1M", condicoes)
total += medir("4 Listas 200K", listas)
total += medir("5 Indices 3M", indices)
total += medir("6 Strings 200K", strings)
total += medir("7 Aninhado 1M", aninhado)

print(f"TOTAL: {total:.6f} s")
