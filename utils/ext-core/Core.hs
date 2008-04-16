module Core where

import Encoding

import List (elemIndex)

data Module 
 = Module AnMname [Tdef] [Vdefg]

data Tdef 
  = Data (Qual Tcon) [Tbind] [Cdef]
  | Newtype (Qual Tcon) [Tbind] Axiom (Maybe Ty)

data Cdef 
  = Constr (Qual Dcon) [Tbind] [Ty]

-- Newtype coercion
type Axiom = (Qual Tcon, [Tbind], (Ty,Ty))

data Vdefg 
  = Rec [Vdef]
  | Nonrec Vdef

newtype Vdef = Vdef (Qual Var,Ty,Exp)

data Exp 
  = Var (Qual Var)
  | Dcon (Qual Dcon)
  | Lit Lit
  | App Exp Exp
  | Appt Exp Ty
  | Lam Bind Exp 	  
  | Let Vdefg Exp
  | Case Exp Vbind Ty [Alt] {- non-empty list -}
  | Cast Exp Ty
  | Note String Exp
  | External String Ty

data Bind 
  = Vb Vbind
  | Tb Tbind

data Alt 
  = Acon (Qual Dcon) [Tbind] [Vbind] Exp
  | Alit Lit Exp
  | Adefault Exp

type Vbind = (Var,Ty)
type Tbind = (Tvar,Kind)

data Ty 
  = Tvar Tvar
  | Tcon (Qual Tcon)
  | Tapp Ty Ty
  | Tforall Tbind Ty 
-- Wired-in coercions:
-- These are primitive tycons in GHC, but in ext-core,
-- we make them explicit, to make the typechecker
-- somewhat more clear. 
  | TransCoercion Ty Ty
  | SymCoercion Ty
  | UnsafeCoercion Ty Ty
  | LeftCoercion Ty
  | RightCoercion Ty

data Kind 
  = Klifted
  | Kunlifted
  | Kopen
  | Karrow Kind Kind
  | Keq Ty Ty

-- A CoercionKind isn't really a Kind at all, but rather,
-- corresponds to an arbitrary user-declared axiom.
-- A tycon whose CoercionKind is (DefinedCoercion <tbs> (from, to))
-- represents a tycon with arity (length tbs), whose kind is
-- (from :=: to) (modulo substituting type arguments.
-- It's not a Kind because a coercion must always be fully applied:
-- whenever we see a tycon that has such a CoercionKind, it must
-- be fully applied if it's to be assigned an actual Kind.
-- So, a CoercionKind *only* appears in the environment (mapping
-- newtype axioms onto CoercionKinds).
-- Was that clear??
data CoercionKind = 
   DefinedCoercion [Tbind] (Ty,Ty)

-- The type constructor environment maps names that are
-- either type constructors or coercion names onto either
-- kinds or coercion kinds.
data KindOrCoercion = Kind Kind | Coercion CoercionKind
  
data Lit = Literal CoreLit Ty
  deriving Eq   -- with nearlyEqualTy 

data CoreLit = Lint Integer
  | Lrational Rational
  | Lchar Char
  | Lstring String 
  deriving Eq

-- Right now we represent module names as triples:
-- (package name, hierarchical names, leaf name)
-- An alternative to this would be to flatten the
-- module namespace, either when printing out
-- Core or (probably preferably) in a 
-- preprocessor.
-- We represent the empty module name (as in an unqualified name)
-- with Nothing.

type Mname = Maybe AnMname
newtype AnMname = M (Pname, [Id], Id)
  deriving (Eq, Ord)
type Pname = Id
type Var = Id
type Tvar = Id
type Tcon = Id
type Dcon = Id

type Qual t = (Mname,t)

qual :: AnMname -> t -> Qual t
qual mn t = (Just mn, t)

unqual :: t -> Qual t
unqual = (,) Nothing

type Id = String

eqKind :: Kind -> Kind -> Bool
eqKind Klifted Klifted = True
eqKind Kunlifted Kunlifted = True
eqKind Kopen Kopen = True
eqKind (Karrow k1 k2) (Karrow l1 l2) = k1 `eqKind` l1
                                   &&  k2 `eqKind` l2
eqKind _ _ = False -- no Keq kind is ever equal to any other...
                   -- maybe ok for now?


splitTyConApp_maybe :: Ty -> Maybe (Qual Tcon,[Ty])
splitTyConApp_maybe (Tvar _) = Nothing
splitTyConApp_maybe (Tcon t) = Just (t,[])
splitTyConApp_maybe (Tapp rator rand) = 
   case (splitTyConApp_maybe rator) of
      Just (r,rs) -> Just (r,rs++[rand])
      Nothing     -> case rator of
                       Tcon tc -> Just (tc,[rand])
                       _       -> Nothing
splitTyConApp_maybe t@(Tforall _ _) = Nothing
           
{- Doesn't expand out fully applied newtype synonyms
   (for which an environment is needed). -}
nearlyEqualTy t1 t2 =  eqTy [] [] t1 t2 
  where eqTy e1 e2 (Tvar v1) (Tvar v2) =
	     case (elemIndex v1 e1,elemIndex v2 e2) of
               (Just i1, Just i2) -> i1 == i2
               (Nothing, Nothing)  -> v1 == v2
               _ -> False
	eqTy e1 e2 (Tcon c1) (Tcon c2) = c1 == c2
        eqTy e1 e2 (Tapp t1a t1b) (Tapp t2a t2b) =
	      eqTy e1 e2 t1a t2a && eqTy e1 e2 t1b t2b
        eqTy e1 e2 (Tforall (tv1,tk1) t1) (Tforall (tv2,tk2) t2) =
	      tk1 `eqKind` tk2 && eqTy (tv1:e1) (tv2:e2) t1 t2 
	eqTy _ _ _ _ = False
instance Eq Ty where (==) = nearlyEqualTy


subKindOf :: Kind -> Kind -> Bool
_ `subKindOf` Kopen = True
(Karrow a1 r1) `subKindOf` (Karrow a2 r2) = 
  a2 `subKindOf` a1 && (r1 `subKindOf` r2)
k1 `subKindOf` k2 = k1 `eqKind` k2  -- doesn't worry about higher kinds

baseKind :: Kind -> Bool
baseKind (Karrow _ _ ) = False
baseKind _ = True

isPrimVar (Just mn,_) = mn == primMname
isPrimVar _ = False

primMname = mkPrimMname "Prim"
errMname  = mkBaseMname "Err"
mkBaseMname,mkPrimMname :: Id -> AnMname
mkBaseMname mn = M (basePkg, ghcPrefix, mn)
mkPrimMname mn = M (primPkg, ghcPrefix, mn)
basePkg = "base"
mainPkg = "main"
primPkg = zEncodeString "ghc-prim"
ghcPrefix = ["GHC"]
mainPrefix = []
baseMname = mkBaseMname "Base"
boolMname = mkPrimMname "Bool"
mainVar = qual mainMname "main"
mainMname = M (mainPkg, mainPrefix, "Main")
wrapperMainMname = Just $ M (mainPkg, mainPrefix, "ZCMain")

tcArrow :: Qual Tcon
tcArrow = (Just primMname, "ZLzmzgZR")

tArrow :: Ty -> Ty -> Ty
tArrow t1 t2 = Tapp (Tapp (Tcon tcArrow) t1) t2


ktArrow :: Kind
ktArrow = Karrow Kopen (Karrow Kopen Klifted)

{- Unboxed tuples -}

maxUtuple :: Int
maxUtuple = 100

tcUtuple :: Int -> Qual Tcon
tcUtuple n = (Just primMname,"Z"++ (show n) ++ "H")

ktUtuple :: Int -> Kind
ktUtuple n = foldr Karrow Kunlifted (replicate n Kopen)

tUtuple :: [Ty] -> Ty
tUtuple ts = foldl Tapp (Tcon (tcUtuple (length ts))) ts 

isUtupleTy :: Ty -> Bool
isUtupleTy (Tapp t _) = isUtupleTy t
isUtupleTy (Tcon tc) = tc `elem` [tcUtuple n | n <- [1..maxUtuple]]
isUtupleTy _ = False

dcUtuple :: Int -> Qual Dcon
-- TODO: Seems like Z2H etc. appears in ext-core files,
-- not $wZ2H etc. Is this right?
dcUtuple n = (Just primMname,"Z" ++ (show n) ++ "H")

isUtupleDc :: Qual Dcon -> Bool
isUtupleDc dc = dc `elem` [dcUtuple n | n <- [1..maxUtuple]]

dcUtupleTy :: Int -> Ty
dcUtupleTy n = 
     foldr ( \tv t -> Tforall (tv,Kopen) t)
           (foldr ( \tv t -> tArrow (Tvar tv) t)
		  (tUtuple (map Tvar tvs)) tvs) 
           tvs
     where tvs = map ( \i -> ("a" ++ (show i))) [1..n] 

utuple :: [Ty] -> [Exp] -> Exp
utuple ts es = foldl App (foldl Appt (Dcon (dcUtuple (length es))) ts) es


