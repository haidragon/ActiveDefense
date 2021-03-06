#include "common.h"
#define CDO_DEVICE_NAME    L"\\Device\\DefenseDevice"
#define CDO_SYB_NAME    L"\\??\\DefenseDevice"

// 从应用层给驱动发送一个字符串。
#define  IOCTL_SEND (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x911,METHOD_BUFFERED, FILE_ANY_ACCESS)
// 从驱动读取一个字符串
#define  IOCTL_RECV (ULONG)CTL_CODE(FILE_DEVICE_UNKNOWN, 0x912,METHOD_BUFFERED, FILE_ANY_ACCESS)
// 应用层关闭信号
#define  IOCTL_CLOSE (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x913,METHOD_BUFFERED, FILE_ANY_ACCESS)
// 应用层开启进程保护
#define  IOCTL_PROCESS_PROTECT (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x914,METHOD_BUFFERED, FILE_ANY_ACCESS)
//应用层关闭进程保护
#define  IOCTL_PROCESS_UNPROTECT (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x915,METHOD_BUFFERED, FILE_ANY_ACCESS)
// 应用层开启进程监控
#define  IOCTL_PROCESS_FILTER (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x916,METHOD_BUFFERED, FILE_ANY_ACCESS)
//应用层关闭进程监控
#define  IOCTL_PROCESS_UNFILTER (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x917,METHOD_BUFFERED, FILE_ANY_ACCESS)
// 应用层开启驱动监控
#define  IOCTL_DRIVER_FILTER (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x918,METHOD_BUFFERED, FILE_ANY_ACCESS)
//应用层关闭驱动监控
#define  IOCTL_DRIVER_UNFILTER (ULONG)CTL_CODE( FILE_DEVICE_UNKNOWN, 0x919,METHOD_BUFFERED, FILE_ANY_ACCESS)

PDEVICE_OBJECT g_pDevObj;//生成的设备对象指针
KEVENT g_kEvent;	//全局事件对象
INT g_Close_Flag = 0;//驱动退出标志位

extern LIST_ENTRY		my_list_head;
extern INT g_isRefuse;//指示进程是否被放行
extern PEPROCESS g_ProcectEProcess;//保护的进程
extern ULONG g_OpDat;

NTSTATUS CreateDevice(PDRIVER_OBJECT pDriverObject)
{
	NTSTATUS Status;
	UNICODE_STRING usDevName, usSymName;

	RtlInitUnicodeString(&usDevName, CDO_DEVICE_NAME);
	//创建一个设备
	Status = IoCreateDevice(pDriverObject, 0, &usDevName, 
		FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, 
		FALSE, &g_pDevObj);
	if (!NT_SUCCESS(Status))//发生错误
	{
		return Status;
	}
	g_pDevObj->Flags |= DO_BUFFERED_IO;

	RtlInitUnicodeString(&usSymName, CDO_SYB_NAME);
	//创建符号链接
	IoDeleteSymbolicLink(&usSymName);//先删除
	Status = IoCreateSymbolicLink(&usSymName, &usDevName);
	if (!NT_SUCCESS(Status))
	{
		IoDeleteDevice(g_pDevObj);
		return Status;
	}
	return STATUS_SUCCESS;
}
VOID UnloadDevice()//删除设备对象及符号链接
{
	UNICODE_STRING cdo_syb = RTL_CONSTANT_STRING(CDO_SYB_NAME);
	IoDeleteSymbolicLink(&cdo_syb);//删除符号链接
	IoDeleteDevice(g_pDevObj);//删除对象

}

NTSTATUS MyDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)//Control分发函数
{
	NTSTATUS Status = STATUS_SUCCESS;
	PIO_STACK_LOCATION irpsp = IoGetCurrentIrpStackLocation(Irp);
	PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
	ULONG inlen = irpsp->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outlen = irpsp->Parameters.DeviceIoControl.OutputBufferLength;
	ULONG code = irpsp->Parameters.DeviceIoControl.IoControlCode;
	
	switch (code)
	{
	case IOCTL_SEND://应用发送信号
		KdPrint(("IOCTL_SEND"));
	
		if (!IsListEmpty(&my_list_head) && inlen==4 )//链表不为空
		{
			g_isRefuse = *(int *)(buffer);//取出应用层返回结果
			PMY_EVENT pEvent = RemoveEventFromList();//从链表删除一个事件

			if (pEvent->pProcessEvent)
			{
				KeSetEvent(pEvent->pProcessEvent, IO_NO_INCREMENT, FALSE);//激活事件,回调进程继续运行,处理程序是否运行
			}
			
			ExFreePool(pEvent);//释放内存
		}
		
		Irp->IoStatus.Information = 0;//写入长度
		break;
	//-----------------------------------------------------------------------------------------------------
	case IOCTL_RECV://应用读取信号
		KdPrint(("IOCTL_RECV"));
	
		while (IsListEmpty(&my_list_head) && g_Close_Flag == 0)//链表为空且标志位为0
		{
			KdPrint(("循环等待,g_Close_Flag:%d", g_Close_Flag));
			KeWaitForSingleObject(&g_kEvent, Executive, KernelMode, FALSE, 0);//等待事件信号  
		}

		if (g_Close_Flag == 0)//不是关闭信号
		{
			KdPrint(("不是关闭信号"));

			PMY_EVENT pEvent = GetEvent();
			RtlCopyMemory(buffer, &pEvent->nType, sizeof(int));//拷贝4个字节的类型值
			
			RtlCopyMemory((PVOID)((char *)buffer+4), &pEvent->nLength, sizeof(int));//拷贝4个字节字符串长度
			RtlCopyMemory((PVOID)((char *)buffer + 8), pEvent->wStr, pEvent->nLength);//拷贝wchar字符串
			//KdPrint(("nType:%d,nLength:%d", pEvent->nType, pEvent->nLength));
			Irp->IoStatus.Information = 8 + pEvent->nLength;//写入长度

			if (pEvent->nType== 2 )//进程退出事件
			{
				pEvent = RemoveEventFromList();//从链表删除一个事件
				ExFreePool(pEvent);//释放内存
			}
		}
		else
		{
			KdPrint(("关闭信号"));
			Irp->IoStatus.Information = 0;//写入长度为0
			g_Close_Flag = 0;//重新设置为0 ,下次应用读取会阻塞
		}

		break;
	//-----------------------------------------------------------------------------------------------------
	case IOCTL_CLOSE:
		KdPrint(("IOCTL_CLOSE"));
		g_Close_Flag = 1;
	
		KeSetEvent(&g_kEvent, IO_NO_INCREMENT, FALSE);//激活事件
		DeleteAllList();//删除链表所有内容
		Irp->IoStatus.Information = 0;
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case IOCTL_PROCESS_PROTECT:
		KdPrint(("IO_PROTECT:%s", buffer));
		Irp->IoStatus.Information=ProcessProcectByName(buffer);
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case	IOCTL_PROCESS_UNPROTECT:
		KdPrint(("IOCTL_PROCESS_UNPROTECT"));
		Irp->IoStatus.Information = UnloadProcessProtect();//关闭进程保护

		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case IOCTL_PROCESS_FILTER:
		KdPrint(("IOCTL_PROCESS_FILTER"));
		Irp->IoStatus.Information = CreateProcessRoutine();//开启进程回调
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case IOCTL_PROCESS_UNFILTER:
		KdPrint(("IOCTL_PROCESS_UNFILTER"));
		g_Close_Flag = 1;

		KeSetEvent(&g_kEvent, IO_NO_INCREMENT, FALSE);//激活事件
		DeleteAllList();//删除链表所有内容
		UnLoadProcessRoutine();//关闭进程回调

		Irp->IoStatus.Information = 0;
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case IOCTL_DRIVER_FILTER:
		KdPrint(("IOCTL_DRIVER_FILTER"));
		Irp->IoStatus.Information = MoudleFilterLoad();//开启进程回调
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	case IOCTL_DRIVER_UNFILTER:
		KdPrint(("IOCTL_DRIVER_UNFILTER"));
		g_Close_Flag = 1;

		KeSetEvent(&g_kEvent, IO_NO_INCREMENT, FALSE);//激活事件
		DeleteAllList();//删除链表所有内容
		
		Irp->IoStatus.Information = MoudleFilterUnLoad();//关闭模块回调
		break;
	//-----------------------------------------------------------------------------------------------------------------------------
	}

	
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}
NTSTATUS MyCreate(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
	KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "MyCreate\r\n"));
	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;

	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Irp->IoStatus.Status;
}
NTSTATUS MyClose(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
	KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "MyClose\r\n"));
	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;

	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Irp->IoStatus.Status;
}

VOID MyUnloadDriver(PDRIVER_OBJECT pDriverObject)
{
	UnloadDevice();//删除设备及符号链接
	UnLoadProcessRoutine();//关闭进程回调

	UnloadProcessProtect();//关闭进程保护
	MoudleFilterUnLoad();//关闭模块监控
	KdPrint(("Unload"));
}
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	KdPrint(("Driver load"));
	NTSTATUS Status;

	//创建设备及符号链接
	Status = CreateDevice(pDriverObject);
	if (!NT_SUCCESS(Status))
	{
		KdPrint(("create Defense device error"));
	}
	else
	{
		KdPrint(("create Defense  device success"));
	}
	//初始化全局事件,同步事件
	KeInitializeEvent(&g_kEvent, SynchronizationEvent, FALSE);
	//链表初始化
	EventListInit();

	//设置分发函数
	pDriverObject->DriverUnload = MyUnloadDriver;
	pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MyDeviceControl;
	pDriverObject->MajorFunction[IRP_MJ_CREATE] = MyCreate;
	pDriverObject->MajorFunction[IRP_MJ_CLOSE] = MyClose;
	pDriverObject->MajorFunction[IRP_MJ_CLEANUP] = MyClose;

	return STATUS_SUCCESS;
}

