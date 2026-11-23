local function medir(nome, fn)
    local inicio = os.clock()
    local resultado = fn()
    local tempo = os.clock() - inicio
    print(nome .. ": " .. string.format("%.6f", tempo) .. " s")
    return tempo
end

local total = 0

total = total + medir("1 Loop 2M", function()
    local x = 0
    for i = 1, 2000000 do x = x + 1 end
    return x
end)

total = total + medir("2 Aritmetica 1M", function()
    local x = 0
    for i = 1, 1000000 do x = (i * 2 + 10) / 2 - 10 end
    return x
end)

total = total + medir("3 Condicoes 1M", function()
    local pares, impares = 0, 0
    for i = 1, 1000000 do
        if i % 2 == 0 then pares = pares + 1
        else impares = impares + 1 end
    end
    return pares + impares
end)

total = total + medir("4 Listas 200K", function()
    local lista = {1,2,3,4,5,6,7,8,9,10}
    local soma = 0
    for i = 1, 200000 do
        for j = 1, #lista do soma = soma + lista[j] end
    end
    return soma
end)

total = total + medir("5 Indices 3M", function()
    local lista = {1,2,3,4,5,6,7,8,9,10}
    local soma = 0
    for i = 1, 3000000 do
        soma = soma + lista[(i % 10) + 1]
    end
    return soma
end)

total = total + medir("6 Strings 200K", function()
    local s = ""
    for i = 1, 200000 do s = s .. "ab" end
    return #s
end)

total = total + medir("7 Aninhado 1M", function()
    local x = 0
    for i = 1, 1000 do
        for j = 1, 1000 do x = x + 1 end
    end
    return x
end)

print("TOTAL: " .. string.format("%.6f", total) .. " s")
