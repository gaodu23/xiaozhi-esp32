 

**把文件放入你的板块目录**

```
//BilibiliFans.h 中修改你的B站UID
class BilibiliFans 
{
private:
    std::string uid_ = "396355825";     // 你的B站UID

```

**添加头文件**

```
//在板块主文件中添加
#include "BilibiliFans.h"
```


**修改原来的IOT出初始化**

```
    void InitializeIot() {
        // 关停原来IOT的实现
        new BilibiliFans();
    }
```

**manuconfig选择板子：**

```
Xiaozhi Assistant -> Board Type -> 你的小智板块
```

**其他设置按照原来的设置**

```
确认小智协议是 mcp模式
```
**还不清楚，看视频或者给我留言**

```
用餐愉快
```

**编译：**

```bash
idf.py build
```