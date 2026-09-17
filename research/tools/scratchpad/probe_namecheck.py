import os
G = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
client = open(os.path.join(G, r"csgo\bin\win64\client.dll"), "rb").read()
engine = open(os.path.join(G, r"bin\win64\engine2.dll"), "rb").read()

rtti_client = [".?AVCPhysicsGameSystem@@", ".?AVCRagdollGameSystem@@", ".?AVCRagdollPoseControlSystem@@",
               ".?AVCRagdollManager@@", ".?AVC_ClientRagdoll@@", ".?AVC_RagdollProp@@"]
rtti_engine = [".?AVCDemoPlayer@@"]
schema_names = ["CEntityIdentity", "m_designerName", "C_BaseEntity", "m_pGameSceneNode", "m_lifeState", "m_flSimulationTime",
                "CGameSceneNode", "m_vecAbsOrigin", "m_angAbsRotation", "CSkeletonInstance", "m_modelState", "CBaseAnimGraph",
                "m_RagdollPose", "m_pClientsideRagdoll", "m_bBuiltRagdoll", "m_bRagdollEnabled", "m_bRagdollClientSide",
                "PhysicsRagdollPose_t", "m_Transforms", "C_RagdollProp", "m_ragPos"]

def count(buf, s):
    n, i, b = 0, 0, s.encode() + b"\0"
    while True:
        i = buf.find(b, i)
        if i < 0: return n
        n += 1; i += 1

for s in rtti_client: print("client RTTI ", s, count(client, s))
for s in rtti_engine: print("engine RTTI ", s, count(engine, s))
for s in schema_names: print("client str  ", s, count(client, s))
