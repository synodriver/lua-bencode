<h1 align="center"><i>✨ lua-bencode ✨ </i></h1>

<h3 align="center">The C extension of bencode</a> </h3>

```lua
local bencode = require("bencode")

print(bencode.loads("i1233e"))
print(bencode.loads("i-1233e"))
print(bencode.loads("6:string"))
print("---------")
for k,v in pairs(bencode.loads("d3:age9:Ein Verne9:interestsi18e4:namel4:book5:movieee")) do
    print(k, v)
    if type(v) == "table" then
        for k1, v1 in pairs(v) do
            print(" ", k1, v1)
        end
    end
end
print("---------")
for k,v in pairs(bencode.loads("l7:contenti42ee")) do
    print(k, v)
end
print(pcall(bencode.loads, "i00e"))

print(bencode.dumps({1,2,3, "3141"}))
print(bencode.dumps({["123"]="ddsaf", ["1243"]=12}, 100000))
```